# ESP32-CAM por Bluetooth (sem Wi-Fi) — TCC v5

## O que tem aqui

| Arquivo | Para que serve |
|---|---|
| `ESP32CAM_BT_V5/ESP32CAM_BT_V5.ino` | Código da ESP32-CAM AI Thinker. Usa **Bluetooth clássico (SPP)**; o Wi-Fi nunca é ligado. Salva fotos e vídeos no cartão microSD da câmera, se houver um. |
| `visualizador_cam.html` | Página que conecta na câmera pelo Bluetooth e mostra fotos e vídeo. Funciona no **Chrome/Edge do computador** e no **Chrome do celular Android**. Salva fotos e vídeos no aparelho. |

A Central e os 4 sensores **não foram alterados**. A câmera é um módulo independente:
não aparece no LCD nem no celular da central, porque a central só aceita os 4 sensores cadastrados
e um único celular.

## Por que Bluetooth clássico e não BLE

Uma foto JPEG em 320×240 tem de 5 a 15 KB. O perfil SPP do Bluetooth clássico transmite mais rápido
que o BLE no ESP32, o que dá para ver vídeo com alguns quadros por segundo em QVGA, e não só fotos
paradas. O número real de quadros por segundo aparece no visualizador e no Monitor Serial.
O iPhone não aceita Bluetooth clássico SPP, então o celular precisa ser Android.

## 1. Gravar o código

1. Arduino IDE com o pacote **esp32 3.x** (o código foi compilado com o 3.3.11).
2. Placa: **AI Thinker ESP32-CAM**. A partição padrão dela já é "Huge APP (3MB No OTA)" — mantenha.
3. A ESP32-CAM não tem USB: use a base **ESP32-CAM-MB** ou um adaptador FTDI
   (5V, GND, U0TXD→RX, U0RXD→TX) com o **IO0 ligado ao GND** durante a gravação.
   Depois de gravar, tire o IO0 do GND e aperte RESET.
4. No Monitor Serial (115200) deve aparecer:
   ```
   [CAM] Camera OK | QVGA | qualidade 12 | PSRAM: sim
   [SD] Cartao OK | 29500 MB livres        (ou: [SD] Sem cartao ...)
   [BT] Bluetooth classico ativo. Procure por ESP32_CAM
   [SISTEMA] Wi-Fi: DESLIGADO (nao e usado)
   ```
5. **Feche o Monitor Serial antes de usar o visualizador no computador** (ele ocupa a porta).

O aviso `'BluetoothSerial' is deprecated` na compilação é normal no pacote 3.x: a biblioteca ainda
funciona e só deixará de vir por padrão na versão 4.0. Não atualize o pacote esp32 para a 4.0.

## 2. Usar no computador (Windows)

1. Configurações → Bluetooth e dispositivos → Adicionar dispositivo → Bluetooth → **ESP32_CAM**.
   No Monitor Serial deve aparecer `[BT] Pareamento CONCLUIDO`.
2. O Windows cria duas portas COM. Veja qual usar em Configurações → Bluetooth →
   **Mais opções de Bluetooth** → aba **Portas COM** → a de direção **Saída**.
3. Abra `visualizador_cam.html` no **Chrome** ou no **Edge** (dois cliques no arquivo), clique em
   **Conectar** e escolha essa porta.

O computador precisa ter Bluetooth (interno ou adaptador USB).

## 3. Usar no celular Android

No celular, o Chrome só libera o Bluetooth para páginas abertas por um link **https://**.
Abrir o arquivo `.html` direto do celular não funciona. Por isso a página precisa ser publicada
uma vez num endereço https. O jeito gratuito é o **GitHub Pages**:

1. Crie uma conta em github.com (se não tiver).
2. Crie um repositório novo e **público**, por exemplo `camera-tcc`.
3. Envie o arquivo `visualizador_cam.html` com o nome **`index.html`**
   (Add file → Upload files).
4. Settings → Pages → Branch: `main`, pasta `/ (root)` → Save.
5. Depois de 1 ou 2 minutos o endereço fica `https://SEU-USUARIO.github.io/camera-tcc/`.

Para conectar:

1. Chrome atualizado (versão 138 ou mais nova; veja em `chrome://version`).
2. Configurações do celular → Bluetooth → parear com **ESP32_CAM**.
3. Abra o endereço https no Chrome → **Conectar** → escolha o ESP32_CAM na lista →
   permita **Dispositivos próximos** se o Chrome pedir.
4. A senha `1234` vai sozinha. Pronto: Foto, Vídeo, Salvar, Gravar.

Se o Chrome do celular disser que não acessa a porta serial, ele está desatualizado.

**Importante:** parear nas Configurações do Bluetooth só registra o aparelho. Nenhum aparelho
Bluetooth consegue abrir uma página no celular sozinho. Sempre abra o link https no Chrome e toque
em **Conectar**. Dicas:

- Se a lista de aparelhos vier vazia, toque em **Mostrar todos os aparelhos** (pede de novo sem filtro).
- Os erros aparecem numa caixa colorida logo abaixo do botão Conectar, com a causa provável.
- Depois da primeira vez o Chrome lembra do aparelho e conecta direto, sem a lista.
- Chrome ⋮ → **Adicionar à tela inicial** cria um ícone para abrir a página como um app.
- A câmera aceita **um aparelho por vez**: se o computador estiver conectado, o celular não entra.

## 4. Guardar fotos e vídeos

### Neste aparelho (computador ou celular)

| Botão | O que faz |
|---|---|
| **Salvar foto** | Baixa a imagem que está na tela (`esp32cam_foto_<data>.jpg`). |
| **Gravar vídeo** / **Parar gravação** | Grava as imagens que chegam. Se o vídeo estiver desligado, ele liga sozinho e desliga ao parar. |
| **Salvar vídeo** | Fica ativo depois de parar a gravação e mostra a duração e o tamanho. **Toque nele para baixar** o vídeo (`.mp4` ou `.webm`; se o navegador devolver vazio, a página monta um `.avi`). |

O vídeo não baixa sozinho ao parar porque o Chrome bloqueia downloads que não vêm de um toque.
Os arquivos vão para a pasta **Downloads** (no celular também).

### No cartão microSD da câmera

- Use um cartão de até 32 GB formatado em **FAT32**, colocado no slot da própria ESP32-CAM.
- A câmera **só libera** os botões "Foto no SD" e "Gravar no SD" depois de montar o cartão
  **e conseguir escrever e ler um arquivo de teste** nele. Sem cartão (ou com cartão com
  defeito), a página mostra "Sem cartão SD na câmera" e os botões ficam desligados.
- Cada foto/vídeo é **reaberto e conferido** antes de a câmera responder. Só então aparece
  "✓ Confirmado no cartão: ..." na página. Se a gravação falhar, aparece um erro e os botões
  do SD são desligados.
- Colocou o cartão com a câmera ligada? Clique em **Verificar cartão**.
- Pastas: `/fotos` (JPG) e `/videos` (AVI, abre no VLC, no Windows e no celular).
- Os nomes usam a data e hora do aparelho conectado (a câmera não tem relógio próprio), por exemplo
  `foto_20261121_153000.jpg`. Sem a hora, ficam numerados: `foto_0001.jpg`.
- A gravação no SD para sozinha se o aparelho desconectar, ao chegar em **10 minutos**, ou se o cartão
  encher. O arquivo é sempre fechado direito.
- Durante a gravação no SD não dá para mudar a resolução (o vídeo ficaria com tamanhos misturados).

Pinos do cartão (modo SPI, já ligados na placa): CLK 14, MOSI 15, MISO 2, CS 13. O flash (GPIO 4)
não é usado pelo cartão.

## 5. Protocolo (para a apresentação)

Segue o mesmo estilo de texto dos sensores (`ID:..;CODE:..;SEQ:..`).

**Aparelho → câmera** (uma linha por comando):

| Comando | Atalho | Ação |
|---|---|---|
| `SENHA 1234` | — | Libera os comandos abaixo até desconectar. 3 erros seguidos → desconecta. |
| `FOTO` | `F` | Envia uma foto. |
| `INICIAR` / `PARAR` | `I` / `P` | Liga / para o vídeo. |
| `FLASH`, `FLASH ON`, `FLASH OFF` | `L` | LED branco da placa. |
| `RES QQVGA\|QVGA\|VGA\|SVGA` | — | Resolução (160×120 até 800×600). |
| `QUAL 10..63` | — | Qualidade JPEG (menor = melhor imagem e mais bytes). |
| `HORA <segundos>` | — | Acerta o relógio (a página envia sozinha ao entrar). |
| `SD` | — | Verifica o cartão (tenta montar de novo). |
| `SDFOTO` | — | Tira uma foto, salva no cartão e mostra na tela. |
| `SDGRAVAR` / `SDPARAR` | — | Inicia / termina um vídeo AVI no cartão. |
| `PING` | — | Responde `PONG`, mesmo sem senha. A página envia a cada 4 s para manter o link ativo. |
| `STATUS` | `S` | Estado, resolução, quadros/s, memória livre e cartão. |
| `AJUDA` | `?` | Lista de comandos. |
| `SAIR` | — | Encerra a senha. |

**Câmera → aparelho:**

- Texto: linhas terminadas em `\r\n` começando com `OK:`, `ERRO:` ou `INFO:`.
- Imagem: a linha `IMG:<bytes>;SEQ:<n>;RES:<largura>x<altura>` seguida de exatamente `<bytes>`
  bytes do JPEG e de `\r\n`. O `SEQ` só avança quando a imagem inteira foi entregue ao Bluetooth
  (mesma regra do `contadorSeq` dos sensores). O visualizador usa o `SEQ` para contar imagens
  perdidas e confere o início do JPEG (`FF D8`) para descartar imagens corrompidas.
- Cartão: `SD:OK;LIVRE_MB:<n>;TOTAL_MB:<n>` ou `SD:AUSENTE` (liga/desliga os botões do SD),
  `SDREC:ON` / `SDREC:OFF` (gravação no cartão) e `OK: SD salvou <arquivo> (<bytes> bytes)`
  (só depois de conferir o arquivo).

## 6. Como o código está organizado

As mesmas 7 seções dos outros códigos v5, sem `delay()`:

- **Núcleo 0 — `tarefaTransmissao`:** única tarefa que escreve no Bluetooth **e no cartão SD**.
  Aplica mudanças de resolução e qualidade entre uma imagem e outra, atende os pedidos do cartão,
  envia as respostas de texto da fila `filaTexto` e captura as imagens. **Uma única captura serve
  para tudo**: a mesma imagem vai para a tela, para a foto no SD e para o vídeo AVI no SD.
- **Núcleo 1 — `loop()`:** detecta conexão e desconexão, lê os comandos, controla a senha, o flash
  e o LED de status, e calcula quadros/s.
- Comunicação entre os dois pelo mesmo padrão da central: contadores de versão
  (`versaoPedidoFoto`, `versaoPedidoFotoSD`, `versaoPedidoSD`, `versaoConfig`), fila do FreeRTOS e
  seção crítica (`muxConfig`).
- **Vídeo AVI:** cabeçalho de 224 bytes gravado zerado no início e reescrito no fim com o número
  de quadros e a taxa real; cada imagem vira um bloco `00dc`; o índice `idx1` fica na PSRAM
  durante a gravação.
- **Relógio:** a página manda a hora local (`HORA`) e a câmera guarda a diferença em relação ao
  `millis()`. As funções de data da biblioteca C (`strftime`, `mktime`) não são usadas porque
  ficariam na memória IRAM, que já está cheia com a câmera e o Bluetooth.

## 7. Problemas comuns

| Sintoma | Causa provável |
|---|---|
| `Camera nao iniciou (0x105)` / `(0x20001)` | Cabo flat da câmera solto ou invertido. |
| Placa reinicia com "Brownout detector was triggered" | Fonte fraca. Use 5 V / 2 A e um capacitor de 470–1000 µF na entrada. Só em último caso mude `DESLIGAR_BROWNOUT` para `true`. |
| Windows mostra "Dispositivo desconhecido" e não pareia | Remova o dispositivo antigo no Windows, aperte RESET na câmera e pareie de novo. O Monitor Serial deve mostrar `[BT] Pareamento CONCLUIDO`. |
| "Failed to open serial port" no visualizador | A porta escolhida é a do cabo USB (CH340) ou está aberta no Monitor Serial do Arduino IDE. Use a porta COM de **Saída** do Bluetooth. |
| Celular: "não acessa a porta serial" | Chrome desatualizado, ou página aberta como arquivo e não pelo link https. |
| Celular: ESP32_CAM não aparece na lista | Pareie primeiro nas Configurações do Bluetooth do celular e permita "Dispositivos próximos" ao Chrome. |
| "Sem cartão SD na câmera" com cartão colocado | Cartão maior que 32 GB, em exFAT/NTFS ou mal encaixado. Formate em FAT32 e clique em **Verificar cartão**. |
| A página diz "Conexão perdida" depois de ~20 s | Ela tenta reconectar sozinha (5 vezes) e religa o vídeo. Para achar a causa, deixe o Monitor Serial aberto na porta **USB** (diferente da porta COM do Bluetooth) e veja a linha `[SAUDE]` a cada 5 s e as linhas `[BT] Porta serial FECHADA`. Se a câmera continuar com `conectado: sim` quando a página cai, o problema é do computador: no Gerenciador de Dispositivos → adaptador Bluetooth → Gerenciamento de energia → desmarque "O computador pode desligar este dispositivo". |
| Vídeo lento | Diminua a resolução (QVGA/QQVGA) ou aumente o número da qualidade (ex.: 20). |
| `ERRO: envio falhou. Video parado` | O Bluetooth ficou mais de 1 s sem conseguir enviar (distância ou interferência). Aproxime e envie `INICIAR`. |
