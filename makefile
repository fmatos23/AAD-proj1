#
# Tomás Oliveira e Silva,  September 2026
#
# Arquiteturas de Alto Desempenho 2026/2027
#
# makefile for the first practical assignment A1 (2026/2027)
#
# makefile automatic variables:
#   $@ is the name of the target
#   $< is the name of the first prerequisite
#   $^ is the list of names of all prerequisites (without duplicates)
#

#
# CUDA installation directory --- /usr/local/cuda or $(CUDA_HOME)
#

CUDA_DIR ?= $(if $(wildcard /usr/local/cuda),/usr/local/cuda,/usr)


#
# OpenCL installation directory (for a NVidia graphics card, sama as CUDA)
#

OPENCL_DIR = $(CUDA_DIR)


#
# CUDA device architecture
#
#   GeForce GTX 1660 Ti --------------------------------------- sm_75
#   GeForce RTX 3050 Ti Laptop GPU ---------------------------- sm_86
#   RTX A2000 Ada --------------------------------------------- sm_86
#   RTX A6000 Ada --------------------------------------------- sm_86
#   RTX 4070 -------------------------------------------------- sm_89
#   NVIDIA RTX PRO 6000 Blackwell Max-Q Workstation Edition --- sm_120
#

# override with: make CUDA_ARCH=sm_XX ...
CUDA_ARCH ?= sm_86


#
# clean up
#

clean:
	rm -f sha1_cpu_tests
	rm -f sha1_cuda_test sha1_cuda_kernel.cubin
	rm -f parse_png
	rm -f sha1_find sha1_find_cuda sha1_search_cuda_kernel.cubin sha1_server sha1_client sha1_find_mpi sha1_find_wasm.js sha1_find_wasm.wasm
	rm -f a.out


#
# test the SHA1_UPDATE macro
#

sha1_cpu_tests:	aad_sha1_cpu_tests.c aad_sha1.h aad_data_types.h aad_utilities.h makefile
	cc -march=native -Wall -Wextra -Wshadow -Werror -O3 $< -o $@

sha1_cuda_test:	aad_sha1_cuda_test.c sha1_cuda_kernel.cubin aad_sha1.h aad_data_types.h aad_utilities.h aad_cuda_utilities.h makefile
	cc -march=native -Wall -Wextra -Wshadow -Werror -O3 $< -o $@ -lcuda


#
# compile the CUDA kernels
#

sha1_cuda_kernel.cubin:	aad_sha1_cuda_kernel.cu aad_sha1.h makefile
	nvcc -arch=$(CUDA_ARCH) --compiler-options -O2,-Wall,-Wextra -I$(CUDA_DIR)/include --cubin $< -o $@


#
# show the structure of PNG files
#
parse_png:	parse_png.c aad_png_crc.h
	cc -march=native -Wall -Wextra -Wshadow -Werror -O3 $< -o $@


#
# A1: search programs
#

SEARCH_DEPS = sha1_search.h sha1_simd.h sha1_engines.h sha1_report.h file_formats.h aad_sha1.h aad_sha1_cpu.h aad_png_crc.h aad_ppm.h aad_data_types.h makefile

sha1_find:	sha1_find.c $(SEARCH_DEPS)
	cc -march=native -Wall -Wextra -Wshadow -Werror -O3 -fopenmp $< -o $@

sha1_find_cuda:	sha1_find.c sha1_cuda_host.h aad_cuda_utilities.h sha1_search_cuda_kernel.cubin $(SEARCH_DEPS)
	cc -march=native -Wall -Wextra -Wshadow -Werror -O3 -fopenmp -DWITH_CUDA -I$(CUDA_DIR)/include $< -o $@ -lcuda

sha1_search_cuda_kernel.cubin:	sha1_search_cuda_kernel.cu aad_sha1.h makefile
	nvcc -arch=$(CUDA_ARCH) --compiler-options -O2,-Wall,-Wextra -I$(CUDA_DIR)/include --cubin $< -o $@

sha1_server:	sha1_server.c sha1_net.h $(SEARCH_DEPS)
	cc -march=native -Wall -Wextra -Wshadow -Werror -O3 $< -o $@

sha1_client:	sha1_client.c sha1_net.h $(SEARCH_DEPS)
	cc -march=native -Wall -Wextra -Wshadow -Werror -O3 -fopenmp $< -o $@

sha1_find_mpi:	sha1_find_mpi.c $(SEARCH_DEPS)
	mpicc -march=native -Wall -Wextra -Wshadow -Werror -O3 -fopenmp $< -o $@

#
# WebAssembly (with SIMD), run with: node sha1_find_wasm.js -e wasm_simd ...
# (WASM_ASYNC_COMPILATION=0 avoids a fetch() problem of emscripten 3.1.5 with node >= 18)
#

sha1_find_wasm.js:	sha1_find.c $(SEARCH_DEPS)
	emcc -O3 -msimd128 -Wall -Wextra -Wshadow -Werror -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -sWASM_ASYNC_COMPILATION=0 $< -o $@

#
# everything that the local tools can build
#

all:	sha1_find sha1_find_cuda sha1_find_mpi sha1_server sha1_client sha1_find_wasm.js parse_png
