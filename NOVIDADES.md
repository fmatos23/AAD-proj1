# Novas versões (opcionais do enunciado)

Este ficheiro descreve o que foi acrescentado ao projeto e como se corre em cada máquina.
Tudo foi testado (ver "O que foi verificado" no fim). Nada do que já existia mudou de comportamento.

| Versão nova | Programa | Ponto do enunciado |
|---|---|---|
| `sha_ni` — instruções SHA1 da Intel/AMD | `sha1_find -e sha_ni` | *optional: SHA1 instructions on Intel/AMD* |
| `neon_sha1` — instruções SHA1 do ARM | `sha1_find -e neon_sha1` (num ARM: telemóvel, Raspberry Pi, Mac M) | *optional: SHA1 instructions on ARM* |
| `opencl` — GPU AMD / Intel / outras | `sha1_find_opencl -e opencl` | *optional: OpenCL implementation* |
| `ispc` — Intel ISPC | `sha1_find_ispc -e ispc` | *optional: Intel ISPC* |
| WebAssembly no browser | `sha1_web.html` | *optional: Web Assembly (+ SIMD)*, e qualquer telemóvel/tablet |

Ficheiros novos: `sha1_shani.h`, `sha1_search_opencl_kernel.cl`, `sha1_opencl_host.h`,
`sha1_ispc.ispc`, `sha1_ispc.h`, `sha1_wasm_main.h`, `sha1_web.html`, `sha1_web_worker.js`,
`verify_files.py`, `NOVIDADES.md`.

Ficheiros alterados: `sha1_engines.h`, `sha1_find.c`, `sha1_simd.h`, `sha1_report.h`,
`sha1_server.c`, `makefile`, `run_benchmarks.sh`, `.gitignore`.

## Correções a problemas que já existiam

1. **WebAssembly não funcionava no Ubuntu 24.04.** O `emscripten` do `apt` (versão 3.1.6)
   gera um `.wasm` vazio (cerca de 60 bytes) para programas com `main(argc,argv)`. O programa
   corria sem fazer nada e sem dar erro. Foi por isso que os CSV do Ryzen e do i7-8700 não
   tinham linhas `wasm_*`. Agora funciona (`sha1_wasm_main.h`).
2. **Vírgulas a mais na última linha dos CSV.** Era consequência do ponto 1: o script
   acrescentava a coluna `gpu_energy_J` mesmo quando a corrida não tinha escrito nenhuma linha.
   O script agora só acrescenta a coluna a uma linha acabada de escrever. Os dois CSV em
   `results/` já estão corrigidos.
3. **`sha1_server` só funcionava com IPv6 ativo.** Agora usa IPv4 se não houver IPv6. Também
   desliga `IPV6_V6ONLY`, para aceitar sempre clientes IPv4.
4. **A compilação falhava (`-Werror`) quando não havia nenhum motor SIMD.** Por exemplo, num
   processador sem AVX ou em WebAssembly sem SIMD. Já não falha.
5. **Nome do processador nos telemóveis/ARM.** O `/proc/cpuinfo` não tem "model name" nesses
   sistemas, e o CSV ficava com `"unknown"`. Agora usa "Hardware" ou "CPU part".

## Instalar (Ubuntu, uma vez por PC)

```bash
sudo apt install build-essential openmpi-bin libopenmpi-dev emscripten nodejs \
                 ispc ocl-icd-opencl-dev opencl-headers clinfo
```

Driver OpenCL da GPU (só o que corresponder ao PC):

```bash
# Ryzen 7 6800H (GPU Radeon 680M integrada): driver "rusticl" do Mesa
sudo apt install mesa-opencl-icd
export RUSTICL_ENABLE=radeonsi        # pôr também no ~/.bashrc
# i7-8700 (GPU Intel UHD 630 integrada, se o PC não tiver placa gráfica)
sudo apt install intel-opencl-icd
# em ambos: dar acesso à GPU ao utilizador (depois fazer logout/login)
sudo usermod -aG render,video $USER
```

Para confirmar que a GPU aparece: `clinfo -l`, ou `make sha1_find_opencl && ./sha1_find_opencl -L`.

## Correr os benchmarks

O script faz tudo; os grupos novos são `sha`, `opencl` e `ispc`. Num PC onde já correste o
resto, basta correr só os grupos novos (e o `wasm`, que agora funciona):

```bash
make clean
./run_benchmarks.sh 60 results/bench_ryzen6800h_novos.csv cpu sha opencl ispc wasm
```

- `cpu` volta a correr os motores de 1 núcleo, agora com o `sha_ni` (só aparece se o
  processador tiver estas instruções: o Ryzen e o i7-1280P têm, o i7-8700 não).
- `sha` corre o `sha_ni` com OpenMP em todas as threads.
- `opencl` corre na GPU. Se aparecer mais de uma plataforma (o Mesa pode mostrar "Clover" e
  "rusticl"), escolhe-se com `AAD_OPENCL_PLATFORM=p AAD_OPENCL_DEVICE=d ./run_benchmarks.sh ...`.
  Os números `p` e `d` são os que o `./sha1_find_opencl -L` mostra.
- `ispc` corre a versão ISPC com 1 thread e com todas as threads.

Antes de medir a GPU, convém confirmar que está a calcular bem. Este comando corre a GPU 10 s
e verifica no CPU cada solução que ela encontrar:

```bash
./sha1_find_opencl -e opencl -T 10 examples/deti_4_2_37_original.pdf
# deve terminar com "..., 0 bad"
```

## WebAssembly no browser (qualquer PC, telemóvel ou tablet)

```bash
make sha1_web
python3 -m http.server 8000
```

- No próprio PC: abrir `http://localhost:8000/sha1_web.html`
- Num telemóvel na **mesma rede Wi-Fi**: abrir `http://IP-DO-PC:8000/sha1_web.html`
  (o IP vê-se com `hostname -I`)

A página faz benchmarks com 1 ou mais "workers" (um por núcleo). Também procura o ficheiro
modificado e deixa descarregá-lo. O botão **Download CSV** dá linhas com as mesmas colunas do
`run_benchmarks.sh`, que se podem juntar aos outros resultados.

## Telemóvel Android "a sério" (NEON e instruções SHA1 do ARM)

Na app **Termux**:

```bash
pkg install clang git
# copiar a pasta do projeto para o telemóvel (git clone, ou termux-setup-storage + cópia)
clang -O3 -march=armv8-a+crypto -fopenmp sha1_find.c -o sha1_find   # se -fopenmp der erro, tirar
./sha1_find                                                            # lista: scalar neon neon_sha1
for e in scalar neon neon_sha1; do ./sha1_find -e $e -b 60 -c bench_telemovel.csv examples/deti_4_2_37_original.pdf; done
./sha1_find -e neon -t 0 -b 60 -c bench_telemovel.csv examples/deti_4_2_37_original.pdf    # todos os núcleos (com OpenMP)
```

Se der `Illegal instruction`, o processador não tem as instruções SHA1. Nesse caso, compilar
com `-march=armv8-a` em vez de `-march=armv8-a+crypto`.

## ISPC noutros processadores

O alvo por omissão é `avx2-i32x8` (8 vias), que serve para os três PCs. Num processador com
AVX-512: `make sha1_find_ispc ISPC_TARGET=avx512skx-x16 ISPC_LANES=16`.

## Verificar ficheiros modificados

```bash
python3 verify_files.py results/*final*
```

Para cada ficheiro, confirma que o SHA1 acaba em 32 bits a zero. Confirma também que o
ficheiro continua válido: CRC de todos os chunks PNG e imagem descomprimível, CRC dos membros
ZIP, `startxref` do PDF, e tamanho e cabeçalho do PPM.

## O que foi verificado

Tudo isto foi testado numa máquina Linux x86-64 (Xeon com AVX-512 e SHA-NI), com Ubuntu 24.04
e as mesmas versões do `apt` (gcc 13, ispc 1.22, emscripten 3.1.6):

- **sha_ni:** `-T` (cada via comparada com a versão escalar e com o SHA1 do ficheiro inteiro)
  nos 4 formatos. Pesquisas reais verificadas com `verify_files.py`, incluindo pesquisas de
  32 bits completas.
- **neon_sha1** e **neon:** compilados para ARM64 com `-march=armv8-a+crypto` e corridos com o
  emulador `qemu-aarch64`, com `-T` nos 4 formatos e pesquisas reais verificadas.
- **opencl:** testado com o POCL (OpenCL a correr no CPU). `-T` deu 0 soluções erradas nos 4
  formatos, e houve pesquisas reais verificadas. O kernel também compila para a arquitetura da
  Radeon 680M (`gfx1035`) e para SPIR. **Não foi possível testar numa GPU real:** é para isso
  que serve o comando `-T` acima.
- **ispc:** `-T` nos 4 formatos com os alvos `sse4.1-i32x4`, `avx2-i32x8`, `avx2-i32x16` e
  `avx512skx-x16`. Pesquisas reais verificadas.
- **WebAssembly:** `node sha1_find_wasm.js` (`-T`, benchmark e pesquisa). A página web foi
  testada no Chrome: benchmark com 2 workers, pesquisa nos 4 formatos, ficheiro escolhido pelo
  utilizador, botão Stop e CSV. Os ficheiros descarregados foram verificados.
- **Versões antigas:** continuam a funcionar (`sha1_cpu_tests`, `-T` dos motores SIMD,
  servidor+clientes, MPI). O código CUDA do host compila com as alterações; o `.cu` não foi
  mexido.
- **`./run_benchmarks.sh 2`** com todos os grupos (exceto `cuda`, por não haver GPU NVIDIA):
  55 linhas, todas com 13 colunas.
