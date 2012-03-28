#
#  Copyright (C) 2011
#  University of Rochester Department of Computer Science
#    and
#  Lehigh University Department of Computer Science and Engineering
# 
# License: Modified BSD
#          Please see the file LICENSE.RSTM for licensing information
#

#
# This makefile is for building the benchmarks using gcc's compiler support,
# Solaris, x86_64, -O3.  Note that it does not use RSTM at all, even though it
# looks like it does.
#
# NB: gcc4.7+ is required for gcctm supoort
#
# Warning: This just handles platform configuration.  Everything else is
#          handled via per-folder Makefiles
#

#
# Compiler config
#
PLATFORM  = gcctm_solaris_x86_64_opt
CXX       = g++
CXXFLAGS += -O3 -ggdb -m64 -march=native -mtune=native -msse2 -mfpmath=sse \
            -fgnu-tm
LDFLAGS  += -lrt -lpthread -m64 -lmtmalloc -fgnu-tm

#
# Options to pass to STM files
#
CXXFLAGS += -DSTM_API_GCCTM
CXXFLAGS += -DSTM_CC_GCC
CXXFLAGS += -DSTM_OS_SOLARIS
CXXFLAGS += -DSTM_CPU_X86
CXXFLAGS += -DSTM_BITS_64
CXXFLAGS += -DSTM_OPT_O3
# NB: next option is incorrect, but it doesn't matter since we aren't
#     actually using RSTM from the gcctm-built benchmarks yet
CXXFLAGS += -DSTM_WS_WORDLOG
