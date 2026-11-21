/*
 * =====================================================================
 *  ESP32-CAM (AI Thinker) | Sistema de Seguranca (TCC) | v5
 *  Camera por BLUETOOTH CLASSICO (SPP) - sem nenhuma dependencia de Wi-Fi
 *  + gravacao de fotos e videos no cartao microSD da propria camera
 * ---------------------------------------------------------------------
 *  Organizacao deste arquivo (mesmas 7 secoes dos outros codigos v5):
 *    1. CONFIGURACAO   pinos da camera, nome Bluetooth, senha, SD, tempos
 *    2. PROTOCOLO      comandos aceitos e formato das respostas/imagens
 *    3. TIPOS E DADOS  estruturas, fila e variaveis globais
 *    4. COMUNICACAO    Bluetooth SPP: tarefa de transmissao (nucleo 0)
 *    5. SISTEMA        camera, cartao SD e gravador de video AVI
 *    6. INTERFACE      comandos, senha, LED de status, flash, Serial
 *    7. setup() / loop()
 *
 *  Por que Bluetooth CLASSICO e nao BLE?
 *    Uma foto JPEG QVGA tem de 5 a 15 KB. O perfil SPP (porta serial
 *    sem fio) transmite bem mais rapido que o BLE no ESP32, o que
 *    permite ver video (alguns quadros por segundo) e nao so fotos.
 *    O Wi-Fi nunca e ligado: nao ha WiFi.begin() nem servidor web.
 *
 *  Quem recebe as imagens?
 *    O arquivo visualizador_cam.html, no Chrome/Edge do computador ou
 *    no Chrome do celular Android. O ESP32-CAM aparece como "ESP32_CAM".
 *
 *  Cartao SD (opcional): a camera so aceita salvar no cartao depois de
 *    montar E escrever um arquivo de teste nele. Cada foto/video salvo e
 *    reaberto e conferido antes de responder "salvou".
 *
 *  Este codigo e independente: a CENTRAL e os 4 sensores nao foram
 *  alterados e continuam funcionando exatamente como antes.
 *
 *  A transmissao e o cartao SD ficam numa tarefa propria (nucleo 0),
 *  porque enviar/gravar uma imagem leva dezenas de milissegundos.
 *  O loop() (nucleo 1) nunca espera por ela. Nenhum delay() e usado.
 *
 *  Arduino IDE: placa "AI Thinker ESP32-CAM" (core esp32 3.x).
 *  Particao: "Huge APP (3MB No OTA)" - e a padrao dessa placa.
 * =====================================================================
 */

#include "esp_camera.h"
#include "BluetoothSerial.h"
#include "FS.h"
#include "SPI.h"
#include "SD.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_system.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Bluetooth desativado. Selecione a placa AI Thinker ESP32-CAM."
#endif

#if !defined(CONFIG_BT_SPP_ENABLED)
#error "Perfil SPP (Bluetooth classico) indisponivel nesta placa."
#endif

// =====================================================================
// 1. CONFIGURACAO
// =====================================================================

// ---------- Identificacao ----------
const char NOME_BT[] = "ESP32_CAM";   // nome que aparece no Bluetooth

// ---------- Senha (mesmo esquema do celular na central) ----------
// Sem a senha, o computador/celular conecta mas nao recebe imagens.
const bool    CAM_EXIGE_SENHA       = true;
const char    CAM_SENHA[]           = "1234";
const uint8_t CAM_TENTATIVAS_MAX    = 3;     // erradas seguidas -> desconecta

// ---------- Pinos da camera (modulo AI Thinker, sensor OV2640) ----------
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ---------- LEDs da placa ----------
const uint8_t PINO_FLASH        = 4;    // LED branco grande (flash)
const uint8_t PINO_LED_STATUS   = 33;   // LED vermelho pequeno (acende em LOW)

// ---------- Cartao microSD (slot da propria placa, modo SPI) ----------
// Modo SPI: simples e rapido o bastante para fotos e video MJPEG.
// Pinos do slot: CLK 14, MOSI(CMD) 15, MISO(D0) 2, CS(D3) 13.
// O GPIO 4 (flash) nao e usado pelo cartao. Formate o cartao em FAT32.
const bool          SD_HABILITADO     = true;
const char          SD_PONTO_MONTAGEM[] = "/sd";
const int8_t        SD_PINO_SCK       = 14;
const int8_t        SD_PINO_MISO      = 2;
const int8_t        SD_PINO_MOSI      = 15;
const int8_t        SD_PINO_CS        = 13;
const uint32_t      SD_FREQUENCIA_HZ  = 20000000;
const char          PASTA_FOTOS[]     = "/fotos";
const char          PASTA_VIDEOS[]    = "/videos";
const char          ARQUIVO_TESTE_SD[] = "/.teste_cam";
const unsigned long SD_VIDEO_MAX_MS   = 10UL * 60UL * 1000UL;  // 10 min por video
const uint32_t      AVI_MAX_QUADROS_PSRAM = 20000;  // indice do video na PSRAM
const uint32_t      AVI_MAX_QUADROS_DRAM  = 1000;   // sem PSRAM

// ---------- Resolucoes permitidas ----------
// Quanto maior a imagem, menos quadros por segundo cabem no Bluetooth.
struct OpcaoResolucao {
  const char* nome;
  framesize_t tamanho;
};

const OpcaoResolucao RESOLUCOES[] = {
  {"QQVGA", FRAMESIZE_QQVGA},   // 160x120
  {"QVGA",  FRAMESIZE_QVGA},    // 320x240  (recomendada)
  {"VGA",   FRAMESIZE_VGA},     // 640x480
  {"SVGA",  FRAMESIZE_SVGA},    // 800x600  (maior aceita)
};
const uint8_t TOTAL_RESOLUCOES     = sizeof(RESOLUCOES) / sizeof(RESOLUCOES[0]);
const uint8_t RESOLUCAO_INICIAL    = 1;   // indice em RESOLUCOES (QVGA)
const uint8_t RESOLUCAO_MAX_SEM_PSRAM = 1; // sem PSRAM, no maximo QVGA

// ---------- Qualidade JPEG (menor numero = melhor imagem e mais bytes) ----------
const int QUALIDADE_INICIAL = 12;
const int QUALIDADE_MIN     = 10;
const int QUALIDADE_MAX     = 63;

// ---------- Tempos ----------
const unsigned long INTERVALO_MIN_QUADRO_MS = 0;    // 0 = o mais rapido possivel
const unsigned long TX_ESPERA_MAX_MS        = 10;   // ciclo maximo da tarefa de envio
const unsigned long ESPERA_APOS_FALHA_MS    = 200;  // camera falhou: espera e tenta
const unsigned long LED_PISCA_MS            = 500;  // pisca enquanto ninguem conecta
const unsigned long ESTATISTICA_MS          = 5000; // quadros/s no Serial
const unsigned long DESCONECTAR_APOS_MS     = 1000; // senha errada: tempo p/ a msg sair

// ---------- Bluetooth ----------
// A biblioteca junta os dados em pacotes de 330 bytes; 3 pacotes por escrita.
const size_t TAMANHO_PEDACO_BT = 990;

// ---------- Alimentacao ----------
// O ESP32-CAM reinicia sozinho ("Brownout detector was triggered") quando a
// fonte nao segura o pico de corrente. A solucao certa e uma fonte de 5 V/2 A
// e um capacitor de 470-1000 uF na entrada. Mude para true so se precisar.
const bool DESLIGAR_BROWNOUT = false;

// =====================================================================
// 2. PROTOCOLO
//   Computador/celular -> camera (texto, uma linha por comando):
//     SENHA <senha>         libera os comandos abaixo ate desconectar
//     FOTO (F)              envia uma foto
//     INICIAR (I)           inicia o video (fotos seguidas)
//     PARAR (P)             para o video
//     FLASH (L)             liga/desliga o flash  (ou FLASH ON / FLASH OFF)
//     RES <nome>            QQVGA, QVGA, VGA ou SVGA
//     QUAL <10-63>          qualidade JPEG (menor = melhor)
//     HORA <segundos>       acerta o relogio (hora local em segundos desde 1970)
//     SD                    verifica o cartao (tenta montar de novo se preciso)
//     SDFOTO                tira uma foto e salva no cartao (e mostra na tela)
//     SDGRAVAR / SDPARAR    inicia / termina um video AVI no cartao
//     STATUS (S)            estado da camera e do cartao
//     AJUDA (?)             lista de comandos
//     SAIR                  encerra a senha
//
//   Camera -> computador/celular:
//     Texto : linhas terminadas em \r\n ("OK: ...", "ERRO: ...", "INFO: ...")
//     Imagem: "IMG:<bytes>;SEQ:<n>;RES:<largura>x<altura>\r\n"
//             seguido de exatamente <bytes> bytes do JPEG e de "\r\n".
//     Cartao: "SD:OK;LIVRE_MB:<n>;TOTAL_MB:<n>"  ou  "SD:AUSENTE"
//             "SDREC:ON" / "SDREC:OFF"   (gravacao de video no cartao)
//             "OK: SD salvou <arquivo> (<bytes> bytes)"   (ja conferido)
//     SEQ so avanca quando a imagem inteira foi entregue ao Bluetooth
//     (mesma regra do contadorSeq dos sensores).
// =====================================================================

#define PREFIXO_IMAGEM "IMG:"

// =====================================================================
// 3. TIPOS E DADOS
// =====================================================================

// ---------- Comunicacao ----------
// Tudo que vai para o Bluetooth passa pela tarefa de transmissao: assim
// um texto nunca e escrito no meio dos bytes de uma imagem.
struct MensagemTexto {
  char texto[112];
};

const UBaseType_t TAMANHO_FILA_TEXTO = 16;

enum ResultadoQuadro : uint8_t { QUADRO_OK, QUADRO_ERRO_ENVIO };

BluetoothSerial SerialBT;
QueueHandle_t   filaTexto = nullptr;
TaskHandle_t    tarefaTransmissaoHandle = nullptr;

// ---------- Camera ----------
bool    cameraPronta   = false;
bool    psramPresente  = false;
uint8_t resolucaoMaxima = TOTAL_RESOLUCOES - 1;

// Pedidos do loop() para a tarefa (versao: cada aumento = um pedido novo).
volatile bool     clienteLiberado    = false;   // conectado e com senha
volatile bool     transmissaoAtiva   = false;   // video ligado
volatile uint32_t versaoPedidoFoto   = 0;       // cada aumento = uma foto
volatile uint32_t versaoConfig       = 0;       // cada aumento = aplicar ajustes
uint8_t           resolucaoPedida    = RESOLUCAO_INICIAL;
int               qualidadePedida    = QUALIDADE_INICIAL;
portMUX_TYPE      muxConfig          = portMUX_INITIALIZER_UNLOCKED;

// Escritas pela tarefa, lidas no loop().
volatile uint8_t  resolucaoAtual   = RESOLUCAO_INICIAL;
volatile int      qualidadeAtual   = QUALIDADE_INICIAL;
volatile uint32_t quadrosEnviados  = 0;
volatile uint32_t bytesEnviados    = 0;
volatile uint32_t contadorSeq      = 0;
volatile uint16_t larguraAtual     = 0;
volatile uint16_t alturaAtual      = 0;

// ---------- Cartao SD ----------
// So a tarefa de transmissao mexe no cartao (nunca duas ao mesmo tempo).
volatile bool     sdPresente         = false;   // montado e testado (tarefa escreve)
volatile bool     sdGravando         = false;   // video AVI aberto (tarefa escreve)
volatile bool     gravarSDPedido     = false;   // loop -> tarefa: quer gravar video
volatile uint32_t versaoPedidoSD     = 0;       // cada aumento = verificar cartao
volatile uint32_t versaoPedidoFotoSD = 0;       // cada aumento = foto no cartao
volatile bool     relogioAcertado    = false;   // recebeu HORA
int64_t           relogioBase        = 0;       // hora local (s) quando millis() era 0

struct DataHora {
  int ano, mes, dia, hora, minuto, segundo;
};

// Gravador AVI (usado somente pela tarefa).
const size_t  AVI_TAMANHO_CABECALHO = 224;
File          aviArquivo;
char          aviNome[48]       = "";
uint32_t      aviQuadros        = 0;
uint32_t      aviBytesDados     = 0;      // bytes dentro da lista 'movi'
uint32_t      aviMaxQuadros     = 0;
uint32_t*     aviIndice         = nullptr; // pares (posicao, tamanho) de cada quadro
unsigned long aviInicio         = 0;
uint16_t      aviLargura        = 0;
uint16_t      aviAltura         = 0;
uint16_t      contadorFoto      = 1;      // nomes sem relogio: foto_0001.jpg
uint16_t      contadorVideo     = 1;

// ---------- Interface (usadas somente no loop) ----------
bool          clienteConectado     = false;
bool          logado               = false;
uint8_t       errosSenha           = 0;
bool          desconectarPendente  = false;
unsigned long desconectarEm        = 0;

char          linhaRecebida[48];
size_t        tamanhoLinha         = 0;
bool          linhaTransbordou     = false;

bool          flashLigado          = false;
bool          ledAceso             = false;
unsigned long ultimoPiscaLed       = 0;

unsigned long ultimaEstatistica    = 0;
uint32_t      quadrosNaEstatistica = 0;
uint32_t      bytesNaEstatistica   = 0;
float         quadrosPorSegundo    = 0;
float         kbPorSegundo         = 0;
uint32_t      pingsRecebidos       = 0;

// ---------- Diagnostico do Bluetooth ----------
// Escritas pelo callback do Bluetooth (tarefa da biblioteca), impressas no loop().
volatile uint32_t sppAberturas       = 0;
volatile uint32_t sppFechamentos     = 0;
volatile int      sppUltimoStatus    = 0;     // status do ultimo fechamento
volatile uint32_t sppCongestionamentos = 0;
volatile bool     sppCongestionado   = false;
volatile uint32_t sppAberturasImpressas   = 0;
volatile uint32_t sppFechamentosImpressos = 0;
const unsigned long SAUDE_MS         = 5000;  // linha de saude no Serial
unsigned long ultimaSaude            = 0;

// ---------- Prototipos ----------
void filaEnviar(const char* texto);
void tarefaAvisar(const char* formato, ...);
void btEnviar(const char* texto);
void btPrintf(const char* formato, ...);
void sdEnviarEstado();
bool aviIniciar();
void aviFinalizar(const char* motivo);
bool aviEscreverQuadro(camera_fb_t* fb);
void sdSalvarFoto(camera_fb_t* fb);
void sdVerificar();

// =====================================================================
// 4. COMUNICACAO (Bluetooth classico, perfil SPP)
// =====================================================================

// Coloca uma linha na fila de envio. Pode ser chamada do loop() ou da tarefa.
void filaEnviar(const char* texto) {
  if (filaTexto == nullptr) {
    return;
  }

  MensagemTexto msg;
  snprintf(msg.texto, sizeof(msg.texto), "%s\r\n", texto);

  if (xQueueSend(filaTexto, &msg, 0) != pdTRUE) {
    Serial.println("[BT] Fila de texto cheia: mensagem descartada.");
    return;
  }

  if (tarefaTransmissaoHandle != nullptr) {
    xTaskNotifyGive(tarefaTransmissaoHandle);
  }
}

// Mensagem gerada pela tarefa (cartao, falhas): Serial + Bluetooth.
void tarefaAvisar(const char* formato, ...) {
  char linha[110];
  va_list argumentos;
  va_start(argumentos, formato);
  vsnprintf(linha, sizeof(linha), formato, argumentos);
  va_end(argumentos);

  Serial.printf("[CAM] -> %s\n", linha);
  filaEnviar(linha);
}

// Resposta para quem esta conectado (aparece tambem no Monitor Serial).
void btEnviar(const char* texto) {
  Serial.printf("[BT] -> %s\n", texto);

  if (clienteConectado) {
    filaEnviar(texto);
  }
}

void btPrintf(const char* formato, ...) {
  char linha[110];
  va_list argumentos;
  va_start(argumentos, formato);
  vsnprintf(linha, sizeof(linha), formato, argumentos);
  va_end(argumentos);

  btEnviar(linha);
}

// Escreve em pedacos. A biblioteca copia cada pedaco para a sua fila
// (32 posicoes) e espera no maximo 1 s se ela estiver cheia.
// Usada somente pela tarefa de transmissao.
bool btEscrever(const uint8_t* dados, size_t tamanho) {
  size_t enviado = 0;

  while (enviado < tamanho) {
    if (!SerialBT.hasClient()) {
      return false;
    }

    size_t parte = tamanho - enviado;
    if (parte > TAMANHO_PEDACO_BT) {
      parte = TAMANHO_PEDACO_BT;
    }

    if (SerialBT.write(dados + enviado, parte) != parte) {
      return false;
    }

    enviado += parte;
  }

  return true;
}

bool btEscreverTexto(const char* texto) {
  return btEscrever(reinterpret_cast<const uint8_t*>(texto), strlen(texto));
}

void enviarTextosPendentes() {
  MensagemTexto msg;

  while (xQueueReceive(filaTexto, &msg, 0) == pdTRUE) {
    if (!btEscreverTexto(msg.texto)) {
      xQueueReset(filaTexto);   // ninguem para receber: descarta o resto
      return;
    }
  }
}

// Aplica resolucao/qualidade entre uma imagem e outra (nunca durante).
void aplicarConfigPendente() {
  static uint32_t versaoAplicada = 0;

  uint32_t versao = versaoConfig;
  if (versao == versaoAplicada) {
    return;
  }

  portENTER_CRITICAL(&muxConfig);
  uint8_t resolucao = resolucaoPedida;
  int qualidade = qualidadePedida;
  portEXIT_CRITICAL(&muxConfig);

  sensor_t* sensor = esp_camera_sensor_get();

  if (sensor != nullptr) {
    sensor->set_framesize(sensor, RESOLUCOES[resolucao].tamanho);
    sensor->set_quality(sensor, qualidade);
  }

  resolucaoAtual = resolucao;
  qualidadeAtual = qualidade;
  versaoAplicada = versao;

  // O quadro que ja estava pronto foi feito com o ajuste antigo: descarta.
  camera_fb_t* antigo = esp_camera_fb_get();
  if (antigo != nullptr) {
    esp_camera_fb_return(antigo);
  }

  Serial.printf("[CAM] Ajuste aplicado: %s | qualidade %d\n",
                RESOLUCOES[resolucao].nome, qualidade);
}

// Envia uma imagem ja capturada: cabecalho + JPEG + "\r\n".
ResultadoQuadro enviarQuadro(camera_fb_t* fb) {
  uint32_t seq = contadorSeq + 1;
  size_t tamanho = fb->len;

  char cabecalho[64];
  snprintf(cabecalho, sizeof(cabecalho), PREFIXO_IMAGEM "%u;SEQ:%lu;RES:%ux%u\r\n",
           (unsigned)tamanho, (unsigned long)seq,
           (unsigned)fb->width, (unsigned)fb->height);

  larguraAtual = fb->width;
  alturaAtual = fb->height;

  bool ok = btEscreverTexto(cabecalho) &&
            btEscrever(fb->buf, tamanho) &&
            btEscreverTexto("\r\n");

  if (!ok) {
    return QUADRO_ERRO_ENVIO;
  }

  contadorSeq = seq;
  quadrosEnviados = quadrosEnviados + 1;
  bytesEnviados = bytesEnviados + tamanho;
  return QUADRO_OK;
}

// Pedidos de cartao: verificar, abrir/fechar o video e limite de tempo.
void atenderCartao(uint32_t& versaoSDAtendida) {
  uint32_t pedidoSD = versaoPedidoSD;
  if (pedidoSD != versaoSDAtendida) {
    versaoSDAtendida = pedidoSD;
    sdVerificar();
  }

  if (gravarSDPedido && !sdGravando) {
    if (!aviIniciar()) {
      gravarSDPedido = false;
    }
  } else if (!gravarSDPedido && sdGravando) {
    aviFinalizar("parada pelo usuario");
  }

  if (sdGravando && (millis() - aviInicio) >= SD_VIDEO_MAX_MS) {
    gravarSDPedido = false;
    aviFinalizar("tempo maximo atingido");
  }
}

// Nucleo 0: unica tarefa que escreve no Bluetooth e no cartao SD.
// Uma unica captura serve para tudo: tela, foto no SD e video no SD.
void tarefaTransmissao(void* parametro) {
  (void)parametro;

  uint32_t versaoFotoAtendida = 0;
  uint32_t versaoFotoSDAtendida = 0;
  uint32_t versaoSDAtendida = 0;

  sdIniciar();   // o Bluetooth ja esta ligado: a camera ja aparece nos aparelhos
  unsigned long ultimoQuadro = 0;

  for (;;) {
    if (cameraPronta) {
      aplicarConfigPendente();
    }

    atenderCartao(versaoSDAtendida);
    enviarTextosPendentes();

    bool clienteOk = clienteLiberado && SerialBT.hasClient();
    uint32_t pedidoFoto = versaoPedidoFoto;
    uint32_t pedidoFotoSD = versaoPedidoFotoSD;

    if (!clienteOk) {
      versaoFotoAtendida = pedidoFoto;       // pedido sem ninguem conectado: descarta
      versaoFotoSDAtendida = pedidoFotoSD;
    }

    bool tempoOk = (millis() - ultimoQuadro) >= INTERVALO_MIN_QUADRO_MS;
    bool querFotoSD = clienteOk && pedidoFotoSD != versaoFotoSDAtendida;
    bool querFoto = clienteOk && (pedidoFoto != versaoFotoAtendida || querFotoSD);
    bool querVideo = clienteOk && transmissaoAtiva && tempoOk;
    bool querAvi = sdGravando && tempoOk;

    if (!cameraPronta || !(querFoto || querVideo || querAvi)) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TX_ESPERA_MAX_MS));
      continue;
    }

    versaoFotoAtendida = pedidoFoto;
    versaoFotoSDAtendida = pedidoFotoSD;
    ultimoQuadro = millis();

    camera_fb_t* fb = esp_camera_fb_get();

    if (fb == nullptr || fb->format != PIXFORMAT_JPEG || fb->len == 0) {
      if (fb != nullptr) {
        esp_camera_fb_return(fb);
      }
      tarefaAvisar("ERRO: falha ao capturar a imagem.");
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ESPERA_APOS_FALHA_MS));
      continue;
    }

    if (querAvi && !aviEscreverQuadro(fb)) {
      gravarSDPedido = false;
      aviFinalizar(aviQuadros >= aviMaxQuadros ? "limite de quadros"
                                               : "falha de escrita no cartao");
    }

    if (querFotoSD) {
      sdSalvarFoto(fb);
    }

    ResultadoQuadro resultado = QUADRO_OK;
    if (querFoto || querVideo) {
      resultado = enviarQuadro(fb);
    }

    esp_camera_fb_return(fb);   // devolve o buffer o quanto antes

    if (resultado == QUADRO_ERRO_ENVIO) {
      // O receptor parou de ler (ou saiu): para o video para nao travar.
      transmissaoAtiva = false;
      tarefaAvisar("ERRO: envio falhou. Video parado (envie INICIAR).");
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ESPERA_APOS_FALHA_MS));
    }
  }
}

void comunicacaoIniciar() {
  filaTexto = xQueueCreate(TAMANHO_FILA_TEXTO, sizeof(MensagemTexto));

  if (filaTexto == nullptr) {
    Serial.println("[ERRO] Nao foi possivel criar a fila de texto.");
  }

  // PAREAMENTO: sem isto a biblioteca RECUSA todo pedido de pareamento
  // ("confirm_request_callback does not exist - refusing pairing") e o
  // Windows mostra "dispositivo desconhecido" / "nao foi possivel conectar".
  // A camera nao tem tela nem teclado, entao o pareamento e automatico
  // ("Just Works"). A protecao das imagens continua sendo a SENHA do protocolo.
  SerialBT.enableSSP(false, false);   // sem entrada e sem saida -> Just Works
  SerialBT.onConfirmRequest([](uint32_t numero) {
    Serial.printf("[BT] Pedido de pareamento (codigo %06lu): aceito\n", (unsigned long)numero);
    SerialBT.confirmReply(true);
  });
  SerialBT.onAuthComplete([](boolean sucesso) {
    Serial.printf("[BT] Pareamento %s\n", sucesso ? "CONCLUIDO" : "FALHOU");
  });

  // Eventos da porta serial Bluetooth, para descobrir por que a conexao cai.
  // So conta aqui: quem imprime e o loop() (nada de Serial nesta tarefa).
  SerialBT.register_callback([](esp_spp_cb_event_t evento, esp_spp_cb_param_t* p) {
    if (evento == ESP_SPP_SRV_OPEN_EVT) {
      sppAberturas = sppAberturas + 1;
    } else if (evento == ESP_SPP_CLOSE_EVT) {
      sppUltimoStatus = (int)p->close.status;
      sppFechamentos = sppFechamentos + 1;
    } else if (evento == ESP_SPP_CONG_EVT) {
      sppCongestionado = p->cong.cong;
      if (p->cong.cong) {
        sppCongestionamentos = sppCongestionamentos + 1;
      }
    }
  });

  // true no 3o parametro: o BLE nao e usado aqui, libera a memoria dele.
  if (!SerialBT.begin(NOME_BT, false, true)) {
    Serial.println("[ERRO] Bluetooth nao iniciou.");
  } else {
    Serial.printf("[BT] Bluetooth classico ativo. Procure por %s\n", NOME_BT);
    Serial.printf("[BT] Endereco: %s\n", SerialBT.getBtAddressString().c_str());
  }

  BaseType_t criada = xTaskCreatePinnedToCore(
      tarefaTransmissao,
      "transmissao",
      8192,
      nullptr,
      1,
      &tarefaTransmissaoHandle,
      0
  );

  if (criada != pdPASS) {
    Serial.println("[ERRO] Tarefa de transmissao nao foi criada.");
  }
}

// =====================================================================
// 5. SISTEMA (camera, cartao SD e gravador de video)
// =====================================================================

// ---------- Camera ----------
bool cameraIniciar() {
  camera_config_t config = {};

  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sccb_sda  = SIOD_GPIO_NUM;
  config.pin_sccb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 20000000;
  config.pixel_format  = PIXFORMAT_JPEG;
  config.jpeg_quality  = QUALIDADE_INICIAL;

  psramPresente = psramFound();

  if (psramPresente) {
    // Inicia na MAIOR resolucao aceita: os buffers ja ficam do tamanho
    // certo e depois da para trocar para qualquer uma menor.
    config.frame_size  = RESOLUCOES[TOTAL_RESOLUCOES - 1].tamanho;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count    = 2;
    config.grab_mode   = CAMERA_GRAB_LATEST;   // sempre a imagem mais nova
    resolucaoMaxima    = TOTAL_RESOLUCOES - 1;
  } else {
    config.frame_size  = RESOLUCOES[RESOLUCAO_MAX_SEM_PSRAM].tamanho;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count    = 1;
    config.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
    resolucaoMaxima    = RESOLUCAO_MAX_SEM_PSRAM;
    Serial.println("[CAM] PSRAM nao encontrada: limite de QVGA.");
  }

  esp_err_t erro = esp_camera_init(&config);

  if (erro != ESP_OK) {
    Serial.printf("[ERRO] Camera nao iniciou (0x%x). Confira o cabo flat da camera.\n",
                  (unsigned)erro);
    return false;
  }

  uint8_t inicial = (RESOLUCAO_INICIAL > resolucaoMaxima) ? resolucaoMaxima : RESOLUCAO_INICIAL;

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_framesize(sensor, RESOLUCOES[inicial].tamanho);
    sensor->set_quality(sensor, QUALIDADE_INICIAL);
  }

  resolucaoPedida = inicial;
  resolucaoAtual = inicial;

  Serial.printf("[CAM] Camera OK | %s | qualidade %d | PSRAM: %s\n",
                RESOLUCOES[inicial].nome, QUALIDADE_INICIAL,
                psramPresente ? "sim" : "nao");
  return true;
}

// Guarda o pedido; a tarefa aplica entre duas imagens.
void cameraPedirAjuste(uint8_t resolucao, int qualidade) {
  portENTER_CRITICAL(&muxConfig);
  resolucaoPedida = resolucao;
  qualidadePedida = qualidade;
  portEXIT_CRITICAL(&muxConfig);

  versaoConfig = versaoConfig + 1;

  if (tarefaTransmissaoHandle != nullptr) {
    xTaskNotifyGive(tarefaTransmissaoHandle);
  }
}

void notificarTarefa() {
  if (tarefaTransmissaoHandle != nullptr) {
    xTaskNotifyGive(tarefaTransmissaoHandle);
  }
}

void cameraPedirFoto() {
  versaoPedidoFoto = versaoPedidoFoto + 1;
  notificarTarefa();
}

// ---------- Cartao SD ----------
// Monta o cartao e cria as pastas. Nao basta montar: o teste de escrita
// abaixo garante que da para salvar de verdade antes de liberar o uso.
SPIClass spiCartao(HSPI);

bool sdMontar() {
  spiCartao.begin(SD_PINO_SCK, SD_PINO_MISO, SD_PINO_MOSI, SD_PINO_CS);
  // Sem cartao o pino de dados (GPIO 2) fica solto e le ruido, que a
  // biblioteca pode confundir com respostas de um cartao. Com pull-up ele
  // le 0xFF, que significa "nenhum cartao respondeu".
  gpio_pullup_en((gpio_num_t)SD_PINO_MISO);
  gpio_pullup_en((gpio_num_t)SD_PINO_CS);
  if (!SD.begin(SD_PINO_CS, spiCartao, SD_FREQUENCIA_HZ, SD_PONTO_MONTAGEM)) {
    SD.end();
    return false;
  }

  if (SD.cardType() == CARD_NONE) {
    SD.end();
    return false;
  }

  if (!SD.exists(PASTA_FOTOS)) {
    SD.mkdir(PASTA_FOTOS);
  }
  if (!SD.exists(PASTA_VIDEOS)) {
    SD.mkdir(PASTA_VIDEOS);
  }
  return true;
}

// Escreve, confere e apaga um arquivo pequeno.
bool sdTestarEscrita() {
  File teste = SD.open(ARQUIVO_TESTE_SD, FILE_WRITE);
  if (!teste) {
    return false;
  }
  size_t escrito = teste.print("ok");
  teste.close();

  File conferir = SD.open(ARQUIVO_TESTE_SD, FILE_READ);
  bool ok = (escrito == 2) && conferir && conferir.size() == 2;
  if (conferir) {
    conferir.close();
  }
  SD.remove(ARQUIVO_TESTE_SD);
  return ok;
}

// ---------- Protecao: a camera SEMPRE liga, com ou sem cartao ----------
// Antes de mexer no cartao, grava uma marca numa memoria que sobrevive ao
// reinicio por software. Se a placa reiniciar no meio do teste, no proximo
// boot a marca ainda esta la: o cartao e desligado e a camera liga normal.
// Desligar e ligar a alimentacao apaga a marca (tenta o cartao de novo).
RTC_NOINIT_ATTR uint32_t marcaTesteSD;
const uint32_t MARCA_TESTANDO_SD = 0x5D7E57AA;
bool sdBloqueado = false;   // o cartao ja derrubou a placa neste ciclo de energia

void sdConferirMarcaDoBoot() {
  esp_reset_reason_t motivo = esp_reset_reason();
  if (motivo != ESP_RST_POWERON && marcaTesteSD == MARCA_TESTANDO_SD) {
    sdBloqueado = true;
    Serial.println("[SD] A placa reiniciou durante o teste do cartao.");
    Serial.println("[SD] Cartao DESLIGADO ate desligar e ligar a camera.");
  }
  marcaTesteSD = 0;
}

// Usada pela tarefa: no inicio e nos comandos SD / STATUS.
bool sdDetectar() {
  if (!SD_HABILITADO || sdBloqueado) {
    return false;
  }

  marcaTesteSD = MARCA_TESTANDO_SD;   // se travar daqui ate o fim, o proximo boot pula o SD

  bool ok;
  if (sdPresente && sdTestarEscrita()) {
    ok = true;
  } else {
    SD.end();   // cartao trocado ou removido: desmonta e tenta de novo
    ok = sdMontar() && sdTestarEscrita();
  }

  marcaTesteSD = 0;
  return ok;
}

// Primeira procura pelo cartao: feita pela tarefa, DEPOIS do Bluetooth
// ligado, para a camera aparecer nos aparelhos mesmo que o cartao falhe.
void sdIniciar() {
  if (!SD_HABILITADO) {
    Serial.println("[SD] Cartao desabilitado no codigo.");
    return;
  }
  if (sdBloqueado) {
    return;
  }

  Serial.println("[SD] Procurando o cartao...");
  sdPresente = sdDetectar();

  if (sdPresente) {
    Serial.printf("[SD] Cartao OK | %lu MB livres\n",
                  (unsigned long)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL)));
  } else {
    Serial.println("[SD] Sem cartao. A camera funciona normal; salvar no SD fica desligado.");
  }
}

// Linha que a pagina usa para ligar/desligar os botoes do cartao.
void sdEnviarEstado() {
  if (!sdPresente) {
    tarefaAvisar("SD:AUSENTE");
    return;
  }
  uint64_t total = SD.totalBytes() / (1024ULL * 1024ULL);
  uint64_t usado = SD.usedBytes() / (1024ULL * 1024ULL);
  tarefaAvisar("SD:OK;LIVRE_MB:%lu;TOTAL_MB:%lu",
               (unsigned long)(total - usado), (unsigned long)total);
}

// Chamada pela tarefa (comandos SD e STATUS).
void sdVerificar() {
  if (sdBloqueado) {
    tarefaAvisar("INFO: cartao desligado (a placa travou ao testa-lo). Desligue e ligue a camera.");
  }
  if (!sdGravando) {   // gravando: o cartao ja esta provado, nao atrapalha
    sdPresente = sdDetectar();
  }
  sdEnviarEstado();
}

// Algo deu errado no cartao: desliga as opcoes de salvar na pagina.
void sdMarcarAusente() {
  sdPresente = false;
  tarefaAvisar("SD:AUSENTE");
}

// ---------- Relogio (sem Wi-Fi e sem bateria de relogio) ----------
// HORA guarda a hora local recebida menos o tempo ligado (millis).
// As funcoes de data da biblioteca C (strftime/mktime) nao sao usadas
// porque ficam na memoria IRAM, que ja esta cheia com camera + Bluetooth.
int64_t relogioAgora() {
  return relogioBase + (int64_t)(millis() / 1000);
}

// Segundos desde 1970 -> dia/mes/ano hora:min:seg (calendario gregoriano).
DataHora dataDeSegundos(int64_t segundos) {
  DataHora d;
  int64_t dias = segundos / 86400;
  int32_t resto = (int32_t)(segundos % 86400);
  d.hora = resto / 3600;
  d.minuto = (resto / 60) % 60;
  d.segundo = resto % 60;

  int64_t z = dias + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  d.dia = (int)(doy - (153 * mp + 2) / 5 + 1);
  d.mes = (int)(mp < 10 ? mp + 3 : mp - 9);
  d.ano = (int)(yoe + era * 400 + (d.mes <= 2 ? 1 : 0));
  return d;
}

// Nome do proximo arquivo. Com relogio: foto_20261121_153000.jpg.
// Sem relogio: foto_0001.jpg, foto_0002.jpg, ... (nunca sobrescreve).
void proximoNome(const char* pasta, const char* prefixo, const char* extensao,
                 uint16_t& contador, char* saida, size_t tamanho) {
  if (relogioAcertado) {
    DataHora data = dataDeSegundos(relogioAgora());
    char base[20];
    snprintf(base, sizeof(base), "%04d%02d%02d_%02d%02d%02d",
             data.ano, data.mes, data.dia, data.hora, data.minuto, data.segundo);

    snprintf(saida, tamanho, "%s/%s_%s.%s", pasta, prefixo, base, extensao);
    for (int n = 2; SD.exists(saida) && n < 100; n++) {
      snprintf(saida, tamanho, "%s/%s_%s_%d.%s", pasta, prefixo, base, n, extensao);
    }
    return;
  }

  do {
    snprintf(saida, tamanho, "%s/%s_%04u.%s", pasta, prefixo, (unsigned)contador, extensao);
    contador++;
  } while (SD.exists(saida) && contador < 9999);
}

// Confere se o arquivo ficou com o tamanho esperado.
bool sdConferirTamanho(const char* nome, uint32_t esperado) {
  File conferir = SD.open(nome, FILE_READ);
  if (!conferir) {
    return false;
  }
  bool ok = (conferir.size() == esperado);
  conferir.close();
  return ok;
}

void sdSalvarFoto(camera_fb_t* fb) {
  if (!sdPresente) {
    tarefaAvisar("ERRO: sem cartao SD na camera.");
    tarefaAvisar("SD:AUSENTE");
    return;
  }

  char nome[48];
  proximoNome(PASTA_FOTOS, "foto", "jpg", contadorFoto, nome, sizeof(nome));

  File arquivo = SD.open(nome, FILE_WRITE);
  if (!arquivo) {
    tarefaAvisar("ERRO: nao consegui criar o arquivo no cartao.");
    sdMarcarAusente();
    return;
  }

  size_t escrito = arquivo.write(fb->buf, fb->len);
  arquivo.close();

  if (escrito != fb->len || !sdConferirTamanho(nome, fb->len)) {
    SD.remove(nome);
    tarefaAvisar("ERRO: a foto NAO foi salva (cartao cheio ou com defeito).");
    sdMarcarAusente();
    return;
  }

  tarefaAvisar("OK: SD salvou %s (%u bytes)", nome, (unsigned)fb->len);
}

// ---------- Gravador de video AVI (MJPEG) ----------
// Formato: RIFF 'AVI ' | LIST 'hdrl' (avih + strl) | LIST 'movi' (quadros
// '00dc', cada um um JPEG) | 'idx1' (indice). O cabecalho e gravado zerado
// no inicio e reescrito no fim com o numero de quadros e a taxa real.

void escrever32(uint8_t*& p, uint32_t valor) {
  p[0] = valor; p[1] = valor >> 8; p[2] = valor >> 16; p[3] = valor >> 24;
  p += 4;
}

void escrever16(uint8_t*& p, uint16_t valor) {
  p[0] = valor; p[1] = valor >> 8;
  p += 2;
}

void escreverCodigo(uint8_t*& p, const char* codigo) {
  memcpy(p, codigo, 4);
  p += 4;
}

void aviMontarCabecalho(uint8_t* cabecalho, uint32_t duracaoMs) {
  memset(cabecalho, 0, AVI_TAMANHO_CABECALHO);

  uint32_t quadros = aviQuadros;
  uint32_t usPorQuadro = quadros > 0 ? (uint32_t)((uint64_t)duracaoMs * 1000 / quadros) : 100000;
  uint32_t taxaMil = quadros > 0 ? (uint32_t)((uint64_t)quadros * 1000000ULL / duracaoMs) : 10000;
  uint32_t tamanhoIndice = 8 + 16 * quadros;
  uint32_t tamanhoArquivo = AVI_TAMANHO_CABECALHO + aviBytesDados + tamanhoIndice;
  uint32_t maiorBuffer = (uint32_t)aviLargura * aviAltura;   // estimativa para JPEG

  uint8_t* p = cabecalho;
  escreverCodigo(p, "RIFF"); escrever32(p, tamanhoArquivo - 8); escreverCodigo(p, "AVI ");
  escreverCodigo(p, "LIST"); escrever32(p, 192); escreverCodigo(p, "hdrl");

  escreverCodigo(p, "avih"); escrever32(p, 56);
  escrever32(p, usPorQuadro);          // microssegundos por quadro
  escrever32(p, 0);                    // bytes/s maximo
  escrever32(p, 0);                    // granularidade
  escrever32(p, 0x10);                 // AVIF_HASINDEX
  escrever32(p, quadros);              // total de quadros
  escrever32(p, 0);                    // quadros iniciais
  escrever32(p, 1);                    // 1 fluxo (video)
  escrever32(p, maiorBuffer);
  escrever32(p, aviLargura);
  escrever32(p, aviAltura);
  p += 16;                             // reservado

  escreverCodigo(p, "LIST"); escrever32(p, 116); escreverCodigo(p, "strl");
  escreverCodigo(p, "strh"); escrever32(p, 56);
  escreverCodigo(p, "vids"); escreverCodigo(p, "MJPG");
  escrever32(p, 0);                    // flags
  escrever16(p, 0); escrever16(p, 0);  // prioridade, idioma
  escrever32(p, 0);                    // quadros iniciais
  escrever32(p, 1000);                 // escala
  escrever32(p, taxaMil);              // taxa (quadros/s x 1000)
  escrever32(p, 0);                    // inicio
  escrever32(p, quadros);              // duracao em quadros
  escrever32(p, maiorBuffer);
  escrever32(p, 0xFFFFFFFF);           // qualidade padrao
  escrever32(p, 0);                    // tamanho da amostra
  escrever16(p, 0); escrever16(p, 0); escrever16(p, aviLargura); escrever16(p, aviAltura);

  escreverCodigo(p, "strf"); escrever32(p, 40);
  escrever32(p, 40);                   // tamanho do BITMAPINFOHEADER
  escrever32(p, aviLargura);
  escrever32(p, aviAltura);
  escrever16(p, 1);                    // planos
  escrever16(p, 24);                   // bits por pixel
  escreverCodigo(p, "MJPG");
  escrever32(p, (uint32_t)aviLargura * aviAltura * 3);
  p += 16;                             // resolucao e cores (zero)

  escreverCodigo(p, "LIST"); escrever32(p, 4 + aviBytesDados); escreverCodigo(p, "movi");
}

bool aviIniciar() {
  if (!sdPresente) {
    tarefaAvisar("ERRO: sem cartao SD na camera.");
    tarefaAvisar("SDREC:OFF");
    tarefaAvisar("SD:AUSENTE");
    return false;
  }

  if (aviIndice == nullptr) {
    aviMaxQuadros = psramPresente ? AVI_MAX_QUADROS_PSRAM : AVI_MAX_QUADROS_DRAM;
    size_t bytes = aviMaxQuadros * 2 * sizeof(uint32_t);
    aviIndice = (uint32_t*)(psramPresente ? ps_malloc(bytes) : malloc(bytes));
    if (aviIndice == nullptr) {
      tarefaAvisar("ERRO: sem memoria para gravar video.");
      tarefaAvisar("SDREC:OFF");
      return false;
    }
  }

  proximoNome(PASTA_VIDEOS, "video", "avi", contadorVideo, aviNome, sizeof(aviNome));
  aviArquivo = SD.open(aviNome, FILE_WRITE);

  uint8_t cabecalho[AVI_TAMANHO_CABECALHO] = {};
  if (!aviArquivo || aviArquivo.write(cabecalho, sizeof(cabecalho)) != sizeof(cabecalho)) {
    if (aviArquivo) {
      aviArquivo.close();
    }
    SD.remove(aviNome);
    tarefaAvisar("ERRO: nao consegui criar o video no cartao.");
    tarefaAvisar("SDREC:OFF");
    sdMarcarAusente();
    return false;
  }

  aviQuadros = 0;
  aviBytesDados = 0;
  aviLargura = 0;
  aviAltura = 0;
  aviInicio = millis();
  sdGravando = true;

  tarefaAvisar("SDREC:ON");
  tarefaAvisar("OK: gravando video no SD: %s", aviNome);
  return true;
}

bool aviEscreverQuadro(camera_fb_t* fb) {
  if (aviQuadros >= aviMaxQuadros) {
    return false;
  }

  if (aviQuadros == 0) {
    aviLargura = fb->width;
    aviAltura = fb->height;
  }

  uint32_t tamanho = fb->len;
  uint8_t bloco[8];
  uint8_t* p = bloco;
  escreverCodigo(p, "00dc");
  escrever32(p, tamanho);

  bool ok = aviArquivo.write(bloco, 8) == 8 &&
            aviArquivo.write(fb->buf, tamanho) == tamanho;

  if (ok && (tamanho & 1)) {               // blocos AVI tem tamanho par
    uint8_t zero = 0;
    ok = aviArquivo.write(&zero, 1) == 1;
  }

  if (!ok) {
    return false;
  }

  aviIndice[aviQuadros * 2] = 4 + aviBytesDados;   // posicao a partir de 'movi'
  aviIndice[aviQuadros * 2 + 1] = tamanho;
  aviBytesDados += 8 + tamanho + (tamanho & 1);
  aviQuadros++;
  return true;
}

// Fecha o video: grava o indice, reescreve o cabecalho e confere o arquivo.
void aviFinalizar(const char* motivo) {
  if (!sdGravando) {
    return;
  }
  sdGravando = false;

  unsigned long duracaoMs = millis() - aviInicio;
  if (duracaoMs == 0) {
    duracaoMs = 1;
  }

  bool ok = true;

  // Indice 'idx1', em blocos de 32 entradas.
  uint8_t bloco[16 * 32];
  uint8_t* p = bloco;
  escreverCodigo(p, "idx1");
  escrever32(p, 16 * aviQuadros);
  ok = ok && aviArquivo.write(bloco, 8) == 8;

  for (uint32_t i = 0; ok && i < aviQuadros; i += 32) {
    uint32_t n = (aviQuadros - i < 32) ? (aviQuadros - i) : 32;
    p = bloco;
    for (uint32_t k = 0; k < n; k++) {
      escreverCodigo(p, "00dc");
      escrever32(p, 0x10);                       // quadro-chave
      escrever32(p, aviIndice[(i + k) * 2]);
      escrever32(p, aviIndice[(i + k) * 2 + 1]);
    }
    ok = aviArquivo.write(bloco, 16 * n) == 16 * n;
  }

  uint8_t cabecalho[AVI_TAMANHO_CABECALHO];
  aviMontarCabecalho(cabecalho, duracaoMs);
  ok = ok && aviArquivo.seek(0) &&
       aviArquivo.write(cabecalho, sizeof(cabecalho)) == sizeof(cabecalho);
  aviArquivo.close();

  uint32_t esperado = AVI_TAMANHO_CABECALHO + aviBytesDados + 8 + 16 * aviQuadros;
  ok = ok && sdConferirTamanho(aviNome, esperado);

  tarefaAvisar("SDREC:OFF");

  if (!ok || aviQuadros == 0) {
    tarefaAvisar("ERRO: video NAO foi salvo direito (%s).", motivo);
    if (!ok) {
      sdMarcarAusente();
    }
    return;
  }

  tarefaAvisar("OK: SD salvou %s (%lu bytes)", aviNome, (unsigned long)esperado);
  tarefaAvisar("INFO: video %lu quadros, %lu s (%s)",
               (unsigned long)aviQuadros, (unsigned long)(duracaoMs / 1000), motivo);
}

// =====================================================================
// 6. INTERFACE
// =====================================================================

// ---------- LED de status e flash ----------
void ledStatusDefinir(bool aceso) {
  ledAceso = aceso;
  digitalWrite(PINO_LED_STATUS, aceso ? LOW : HIGH);   // acende em LOW
}

// Pisca esperando conexao; aceso com alguem conectado.
void ledStatusAtualizar() {
  if (clienteConectado) {
    if (!ledAceso) {
      ledStatusDefinir(true);
    }
    return;
  }

  unsigned long agora = millis();
  if ((agora - ultimoPiscaLed) >= LED_PISCA_MS) {
    ultimoPiscaLed = agora;
    ledStatusDefinir(!ledAceso);
  }
}

void flashDefinir(bool ligado) {
  flashLigado = ligado;
  digitalWrite(PINO_FLASH, ligado ? HIGH : LOW);
}

// ---------- Mensagens ----------
void enviarAjuda() {
  btEnviar("Comandos: FOTO (F), INICIAR (I), PARAR (P), FLASH (L),");
  btEnviar("RES QQVGA|QVGA|VGA|SVGA, QUAL 10-63, STATUS (S), AJUDA (?), SAIR");
  btEnviar("Cartao: SD, SDFOTO, SDGRAVAR, SDPARAR | Relogio: HORA <segundos>");
  btEnviar("Teste de conexao: PING (responde PONG)");

  if (CAM_EXIGE_SENHA && !logado) {
    btEnviar("Antes de tudo envie: SENHA <sua senha>");
  }
}

void pedirVerificacaoSD() {
  versaoPedidoSD = versaoPedidoSD + 1;
  notificarTarefa();
}

void enviarStatus() {
  btPrintf("INFO: camera %s | video %s | flash %s",
           cameraPronta ? "OK" : "COM DEFEITO",
           transmissaoAtiva ? "LIGADO" : "PARADO",
           flashLigado ? "LIGADO" : "DESLIGADO");
  btPrintf("INFO: %s | qualidade %d | %.1f quadros/s | %.1f KB/s",
           RESOLUCOES[resolucaoAtual].nome, (int)qualidadeAtual,
           quadrosPorSegundo, kbPorSegundo);
  btPrintf("INFO: imagens enviadas %lu | memoria livre %lu bytes",
           (unsigned long)quadrosEnviados, (unsigned long)ESP.getFreeHeap());
  btPrintf("INFO: gravando no SD: %s | relogio %s",
           sdGravando ? "SIM" : "nao", relogioAcertado ? "acertado" : "sem hora");
  pedirVerificacaoSD();   // a tarefa responde com a linha SD:...
}

// ---------- Senha (mesma logica do celular na central) ----------
bool liberadoParaImagens() {
  return !CAM_EXIGE_SENHA || logado;
}

void atualizarLiberacao() {
  clienteLiberado = clienteConectado && liberadoParaImagens();
}

void fazerLogin(const char* senha) {
  if (!CAM_EXIGE_SENHA) {
    btEnviar("OK: senha nao e necessaria.");
    return;
  }

  if (strcasecmp(senha, CAM_SENHA) == 0) {   // o texto ja chega em maiusculas
    logado = true;
    errosSenha = 0;
    atualizarLiberacao();
    btEnviar("OK: senha correta. Imagens liberadas.");
    return;
  }

  errosSenha++;
  btPrintf("ERRO: senha incorreta (%u de %u).",
           (unsigned)errosSenha, (unsigned)CAM_TENTATIVAS_MAX);

  if (errosSenha >= CAM_TENTATIVAS_MAX) {
    btEnviar("ERRO: muitas tentativas. Desconectando.");
    desconectarPendente = true;
    desconectarEm = millis() + DESCONECTAR_APOS_MS;
  }
}

bool autorizado() {
  if (liberadoParaImagens()) {
    return true;
  }
  btEnviar("ERRO: envie SENHA <sua senha> antes.");
  return false;
}

// ---------- Comandos ----------
// Tira espacos das pontas e passa para maiusculas.
void normalizarTexto(char* texto) {
  char* inicio = texto;
  while (*inicio == ' ' || *inicio == '\t') {
    inicio++;
  }
  memmove(texto, inicio, strlen(inicio) + 1);

  size_t n = strlen(texto);
  while (n > 0 && (texto[n - 1] == ' ' || texto[n - 1] == '\t')) {
    n--;
    texto[n] = '\0';
  }

  for (size_t i = 0; i < n; i++) {
    texto[i] = (char)toupper((unsigned char)texto[i]);
  }
}

bool textoIgual(const char* texto, const char* opcao1, const char* opcao2) {
  return strcmp(texto, opcao1) == 0 || strcmp(texto, opcao2) == 0;
}

// Devolve o texto depois do comando ("RES VGA" -> "VGA"), ou nullptr.
const char* argumentoDe(const char* texto, const char* comando) {
  size_t n = strlen(comando);
  if (strncmp(texto, comando, n) != 0 || (texto[n] != ' ' && texto[n] != '\0')) {
    return nullptr;
  }
  const char* argumento = texto + n;
  while (*argumento == ' ') {
    argumento++;
  }
  return argumento;
}

bool gravacaoSDAtiva() {
  return gravarSDPedido || sdGravando;
}

void comandoResolucao(const char* nome) {
  if (gravacaoSDAtiva()) {
    btEnviar("ERRO: pare a gravacao do SD antes de mudar a resolucao.");
    return;
  }

  for (uint8_t i = 0; i < TOTAL_RESOLUCOES; i++) {
    if (strcmp(nome, RESOLUCOES[i].nome) != 0) {
      continue;
    }
    if (i > resolucaoMaxima) {
      btPrintf("ERRO: sem PSRAM a maior resolucao e %s.", RESOLUCOES[resolucaoMaxima].nome);
      return;
    }
    cameraPedirAjuste(i, qualidadePedida);
    btPrintf("OK: resolucao %s.", RESOLUCOES[i].nome);
    return;
  }
  btEnviar("ERRO: use RES QQVGA, QVGA, VGA ou SVGA.");
}

void comandoQualidade(const char* valor) {
  int qualidade = atoi(valor);
  if (qualidade < QUALIDADE_MIN || qualidade > QUALIDADE_MAX) {
    btPrintf("ERRO: qualidade de %d a %d (menor = melhor).", QUALIDADE_MIN, QUALIDADE_MAX);
    return;
  }
  cameraPedirAjuste(resolucaoPedida, qualidade);
  btPrintf("OK: qualidade %d.", qualidade);
}

void comandoFlash(const char* argumento) {
  if (argumento == nullptr || argumento[0] == '\0') {
    flashDefinir(!flashLigado);
  } else if (strcmp(argumento, "ON") == 0 || strcmp(argumento, "LIGAR") == 0) {
    flashDefinir(true);
  } else if (strcmp(argumento, "OFF") == 0 || strcmp(argumento, "DESLIGAR") == 0) {
    flashDefinir(false);
  } else {
    btEnviar("ERRO: use FLASH, FLASH ON ou FLASH OFF.");
    return;
  }
  btPrintf("OK: flash %s.", flashLigado ? "LIGADO" : "DESLIGADO");
}

// HORA <segundos>: a pagina manda a hora local do aparelho, porque a
// camera nao tem Wi-Fi para buscar a hora na internet.
void comandoHora(const char* valor) {
  long long segundos = atoll(valor);
  if (segundos < 1600000000LL) {
    btEnviar("ERRO: use HORA <segundos desde 1970>.");
    return;
  }

  relogioBase = segundos - (int64_t)(millis() / 1000);
  relogioAcertado = true;

  DataHora data = dataDeSegundos(segundos);
  btPrintf("OK: hora acertada %02d/%02d/%04d %02d:%02d:%02d",
           data.dia, data.mes, data.ano, data.hora, data.minuto, data.segundo);
}

// Comandos do cartao: so deixa pedir se o cartao foi confirmado.
bool exigirCartao() {
  if (sdPresente) {
    return true;
  }
  btEnviar("ERRO: sem cartao SD na camera. Envie SD para verificar de novo.");
  btEnviar("SD:AUSENTE");
  return false;
}

void tratarComando(char* texto) {
  normalizarTexto(texto);

  if (texto[0] == '\0') {
    return;
  }

  // PING da pagina a cada poucos segundos: mantem o link ativo e prova
  // que a camera esta viva. Nao exige senha e nao enche o Monitor Serial.
  if (strcmp(texto, "PING") == 0) {
    pingsRecebidos++;
    if (clienteConectado) {
      filaEnviar("PONG");
    }
    return;
  }

  // A senha nunca aparece no Serial.
  const char* senha = argumentoDe(texto, "SENHA");
  if (senha != nullptr) {
    Serial.println("[BT] <- SENHA ****");
    fazerLogin(senha);
    return;
  }

  Serial.printf("[BT] <- %s\n", texto);

  const char* argumento = nullptr;

  if (textoIgual(texto, "AJUDA", "?")) {
    enviarAjuda();
  } else if (textoIgual(texto, "SAIR", "LOGOUT")) {
    logado = false;
    transmissaoAtiva = false;
    gravarSDPedido = false;
    notificarTarefa();
    atualizarLiberacao();
    btEnviar("OK: senha encerrada. Imagens bloqueadas.");
  } else if (!autorizado()) {
    return;   // os comandos abaixo exigem a senha
  } else if (textoIgual(texto, "STATUS", "S")) {
    enviarStatus();
  } else if ((argumento = argumentoDe(texto, "HORA")) != nullptr) {
    comandoHora(argumento);
  } else if (strcmp(texto, "SD") == 0) {
    pedirVerificacaoSD();
  } else if (!cameraPronta) {
    btEnviar("ERRO: camera com defeito. Confira o cabo e reinicie.");
  } else if (textoIgual(texto, "FOTO", "F")) {
    cameraPedirFoto();
  } else if (textoIgual(texto, "INICIAR", "I")) {
    transmissaoAtiva = true;
    notificarTarefa();
    btEnviar("OK: video iniciado.");
  } else if (textoIgual(texto, "PARAR", "P")) {
    transmissaoAtiva = false;
    btEnviar("OK: video parado.");
  } else if (strcmp(texto, "SDFOTO") == 0) {
    if (exigirCartao()) {
      versaoPedidoFotoSD = versaoPedidoFotoSD + 1;
      notificarTarefa();
    }
  } else if (strcmp(texto, "SDGRAVAR") == 0) {
    if (gravacaoSDAtiva()) {
      btEnviar("INFO: ja esta gravando no SD.");
    } else if (exigirCartao()) {
      gravarSDPedido = true;    // a tarefa abre o arquivo e responde SDREC:ON
      notificarTarefa();
    }
  } else if (strcmp(texto, "SDPARAR") == 0) {
    if (!gravacaoSDAtiva()) {
      btEnviar("INFO: nao esta gravando no SD.");
      btEnviar("SDREC:OFF");
    } else {
      gravarSDPedido = false;   // a tarefa fecha, confere e responde
      notificarTarefa();
    }
  } else if (strcmp(texto, "L") == 0) {
    comandoFlash(nullptr);
  } else if ((argumento = argumentoDe(texto, "FLASH")) != nullptr) {
    comandoFlash(argumento);
  } else if ((argumento = argumentoDe(texto, "RES")) != nullptr) {
    comandoResolucao(argumento);
  } else if ((argumento = argumentoDe(texto, "QUAL")) != nullptr) {
    comandoQualidade(argumento);
  } else {
    btEnviar("ERRO: comando desconhecido. Envie AJUDA.");
  }
}

// Le o Bluetooth sem esperar: junta os bytes ate o fim da linha.
void lerComandos() {
  while (SerialBT.available() > 0) {
    char c = (char)SerialBT.read();

    if (c == '\r' || c == '\n') {
      if (linhaTransbordou) {
        btEnviar("ERRO: comando muito longo.");
      } else if (tamanhoLinha > 0) {
        linhaRecebida[tamanhoLinha] = '\0';
        tratarComando(linhaRecebida);
      }
      tamanhoLinha = 0;
      linhaTransbordou = false;
      continue;
    }

    if (tamanhoLinha < sizeof(linhaRecebida) - 1) {
      linhaRecebida[tamanhoLinha++] = c;
    } else {
      linhaTransbordou = true;
    }
  }
}

// ---------- Conexao ----------
// Detecta quem conectou/saiu e reinicia a sessao (igual ao celular na central).
void conexaoAtualizar() {
  bool conectadoAgora = SerialBT.hasClient();

  if (conectadoAgora && !clienteConectado) {
    clienteConectado = true;
    logado = false;
    errosSenha = 0;
    desconectarPendente = false;
    transmissaoAtiva = false;
    tamanhoLinha = 0;
    linhaTransbordou = false;
    atualizarLiberacao();

    Serial.println("[BT] Cliente conectado.");
    btEnviar("=== ESP32-CAM (TCC) ===");
    enviarAjuda();
  } else if (!conectadoAgora && clienteConectado) {
    clienteConectado = false;
    logado = false;
    transmissaoAtiva = false;
    gravarSDPedido = false;   // ninguem poderia parar depois: fecha o video
    notificarTarefa();
    atualizarLiberacao();
    flashDefinir(false);      // ninguem olhando: apaga o flash
    if (filaTexto != nullptr) {
      xQueueReset(filaTexto);
    }
    Serial.println("[BT] Cliente desconectado.");
  }

  // Muitas senhas erradas: desconecta depois de a mensagem sair.
  if (desconectarPendente && (long)(millis() - desconectarEm) >= 0) {
    desconectarPendente = false;
    if (clienteConectado) {
      Serial.println("[BT] Muitas senhas erradas: desconectando.");
      SerialBT.disconnect();
    }
  }
}

// ---------- Monitor Serial ----------
// Quadros por segundo e KB/s, so enquanto o video estiver ligado.
void estatisticaAtualizar() {
  unsigned long agora = millis();
  unsigned long decorrido = agora - ultimaEstatistica;

  if (decorrido < ESTATISTICA_MS) {
    return;
  }

  uint32_t quadros = quadrosEnviados;
  uint32_t bytes = bytesEnviados;

  quadrosPorSegundo = (quadros - quadrosNaEstatistica) * 1000.0f / decorrido;
  kbPorSegundo = (bytes - bytesNaEstatistica) / 1.024f / decorrido;

  quadrosNaEstatistica = quadros;
  bytesNaEstatistica = bytes;
  ultimaEstatistica = agora;

  if (transmissaoAtiva) {
    Serial.printf("[CAM] Video: %.1f quadros/s | %.1f KB/s | %ux%u | SEQ=%lu\n",
                  quadrosPorSegundo, kbPorSegundo,
                  (unsigned)larguraAtual, (unsigned)alturaAtual,
                  (unsigned long)contadorSeq);
  }
}

// ---------- Diagnostico da conexao ----------
// Eventos do Bluetooth e uma linha de saude a cada 5 s, com ou sem video.
// Se a pagina disser "Conexao perdida" e aqui continuar "conectado: sim",
// a queda foi do lado do computador/celular, nao da camera.
void diagnosticoAtualizar() {
  uint32_t aberturas = sppAberturas;
  if (aberturas != sppAberturasImpressas) {
    sppAberturasImpressas = aberturas;
    Serial.printf("[BT] Porta serial aberta pelo aparelho (vez %lu)\n", (unsigned long)aberturas);
  }

  uint32_t fechamentos = sppFechamentos;
  if (fechamentos != sppFechamentosImpressos) {
    sppFechamentosImpressos = fechamentos;
    Serial.printf("[BT] Porta serial FECHADA (vez %lu, status %d) apos %lu s ligada\n",
                  (unsigned long)fechamentos, (int)sppUltimoStatus,
                  (unsigned long)(millis() / 1000));
  }

  unsigned long agora = millis();
  if ((agora - ultimaSaude) < SAUDE_MS) {
    return;
  }
  ultimaSaude = agora;

  Serial.printf("[SAUDE] %lus | conectado: %s | senha: %s | video: %s | PINGs %lu | "
                "congest. %lu%s | heap %lu (min %lu)\n",
                (unsigned long)(agora / 1000),
                SerialBT.hasClient() ? "sim" : "nao",
                logado ? "ok" : "nao",
                transmissaoAtiva ? "ligado" : "parado",
                (unsigned long)pingsRecebidos,
                (unsigned long)sppCongestionamentos,
                sppCongestionado ? " (AGORA)" : "",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap());
}

const char* motivoReinicio(esp_reset_reason_t motivo) {
  switch (motivo) {
    case ESP_RST_POWERON:  return "ligou na energia";
    case ESP_RST_SW:       return "reinicio por software";
    case ESP_RST_PANIC:    return "TRAVOU (erro no programa)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "TRAVOU (watchdog)";
    case ESP_RST_BROWNOUT: return "QUEDA DE TENSAO (fonte fraca)";
    case ESP_RST_EXT:      return "botao RESET";
    case ESP_RST_DEEPSLEEP:return "acordou do deep sleep";
    default:               return "desconhecido";
  }
}

// =====================================================================
// 7. SETUP / LOOP
// =====================================================================

// Mostra a etapa e espera o texto sair pela serial ANTES de continuar:
// se a placa reiniciar, a ultima etapa impressa e a que falhou.
void marcarEtapa(const char* texto) {
  Serial.printf("[BOOT] %s\n", texto);
  Serial.flush();
}

void setup() {
  if (DESLIGAR_BROWNOUT) {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  }

  Serial.begin(115200);

  Serial.println();
  Serial.println("================================");
  Serial.println("   ESP32-CAM BLUETOOTH V5");
  Serial.println("================================");
  Serial.printf("[BOOT] Motivo do ultimo reinicio: %d (%s)\n",
                (int)esp_reset_reason(), motivoReinicio(esp_reset_reason()));
  Serial.flush();

  pinMode(PINO_FLASH, OUTPUT);
  flashDefinir(false);
  pinMode(PINO_LED_STATUS, OUTPUT);
  ledStatusDefinir(false);

  // O cartao derrubou a placa no boot anterior? Entao liga sem ele.
  sdConferirMarcaDoBoot();

  marcarEtapa("1/2 iniciando a camera...");
  cameraPronta = cameraIniciar();

  // O cartao e procurado pela tarefa de transmissao, DEPOIS do Bluetooth.
  marcarEtapa("2/2 ligando o Bluetooth...");
  comunicacaoIniciar();

  Serial.println("[SISTEMA] Wi-Fi: DESLIGADO (nao e usado)");
  Serial.printf("[SISTEMA] Senha exigida: %s\n", CAM_EXIGE_SENHA ? "sim" : "nao");
  Serial.println("[SISTEMA] CAMERA INICIADA");
}

void loop() {
  conexaoAtualizar();      // conectou / desconectou
  lerComandos();           // comandos do computador/celular
  ledStatusAtualizar();    // LED vermelho
  estatisticaAtualizar();  // quadros/s no Serial
  diagnosticoAtualizar();  // eventos do Bluetooth e linha de saude
}
