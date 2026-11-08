# SoH 9.3.1-nx2 — Bloom experimental (Switch)
 
**Objetivo:** rodar Bloom diretamente no Nintendo Switch / Switch Lite, sem streaming e sem ReShade instalado no Switch.

A branch `switch-bloom-experimental` contem o shader OpenGL, o aplicador e a compilacao automatica pelo GitHub Actions, usando o codigo-fonte de `timschneeb/Shipwright-Switch` tag `9.3.1-nx2`.

## Como obter o NRO

1. Abra a aba **Actions** deste repositorio GitHub.
2. Escolha **Compilar SoH Switch com Bloom**.
3. Acesse a execucao mais recente da branch `switch-bloom-experimental`.
4. Se a compilacao estiver com o indicador verde, baixe o artefato **SoH-Switch-Bloom-Experimental**.
5. Descompacte-o e copie `soh.nro` para `SD:/switch/soh/soh.nro`, substituindo **apenas depois de fazer backup** do NRO original.
6. Deixe os arquivos originais `soh.o2r` e `oot.o2r` na pasta. Eles **nao** fazem parte do artefato.

Se nao aparecer o fluxo em Actions, confira se Actions esta habilitado para o repositorio e que a branch da compilacao e `switch-bloom-experimental`. Pode ser necessario executar o fluxo pela aba Actions (workflow_dispatch). 

## Estado / limitacoes

- **Bloom:** shader experimental de pos-processamento multi-pass (1/4 de resolucao), intensidade 1.20 do preset enviado.
- **MXAO:** nao implementado, porque depende de buffer de profundidade.
- **Lightroom:** nao implementado, porque os ajustes globais enviados estavam neutros.
- O codigo **nao carrega** `.fx`/ReShadePreset.ini no Switch.
- O patch ainda precisa ser validado em um console real: pode falhar ao compilar, exibir erro grafico ou reduzir FPS.
- A pos-processamento age sobre o quadro final e pode afetar o HUD.
- O pacote **nao contem ROM** e usa seus arquivos originais do SoH.

## Restaurar original

Recopie o `soh.nro` original para `SD:/switch/soh/` se aparecer tela preta, erro, glitch ou perda de desempenho. Seus saves e `.o2r` nao precisam ser apagados.

## Fontes

- https://github.com/timschneeb/Shipwright-Switch/releases/tag/9.3.1-nx2
- https://github.com/timschneeb/Shipwright-Switch/blob/9.3.1-nx2/docs/BUILDING.md
