# Integração CedarC com Jellyfin na Orange Pi 4A

Projeto experimental de processamento de vídeo com aceleração
por hardware CedarC no Jellyfin, utilizando a Orange Pi 4A
com processador Allwinner T527.

## Ambiente de referência

- Placa: Orange Pi 4A
- Sistema: Debian 13 (Trixie)
- Imagem: AurealNix CLI v0.5
- Kernel: 6.18.55-g3f67628a8bff
- Arquitetura: ARM64
- Python: 3.13.5
- PyAV: 19.0.2
- Dispositivo: /dev/cedar_dev

## Arquitetura

Jellyfin
  -> Wrapper FFmpeg
  -> Seletor CedarC
  -> Adaptador V6.0.8
  -> Controlador V5.4
  -> Feeder V5.4
  -> Muxer nativo CedarC
  -> FFmpeg / HLS

Existe fallback para o FFmpeg original do Jellyfin quando
o processamento CedarC não é selecionado.

## Compilação

Em um sistema compatível com os headers e bibliotecas CedarC:

    ./scripts/verify.sh
    ./scripts/build.sh
    ./scripts/package.sh

## Limitações

- Projeto experimental.
- Requer driver e bibliotecas CedarC compatíveis.
- Os caminhos de instalação ainda são específicos do ambiente original.
- Nem todos os codecs, resoluções, legendas e formatos HLS foram validados.
- Remux não significa transcodificação por hardware.
- O pacote ainda depende de um ambiente Python e bibliotecas externas.
- Não substitua uma instalação Jellyfin funcional sem testes prévios.

## Projeto de origem

Imagem-base:
https://github.com/ut-slayer/orangepi-4a-mainline

## Licenciamento

A revisão de autoria, licenças e componentes de terceiros
ainda está pendente.
