#
# Add DDE Linux 2.6 headers to standard include search paths
#
REP_INC_DIR += include/dde_linux26 include/dde_linux26-x86

#
# Add Linux 2.6 headers to standard include search paths
#
REP_INC_DIR += include/linux26 include/linux26-x86

#
# i386 hack in 2.6.20.19
#
REP_INC_DIR += include/linux26-x86/asm/mach-default

CC_OPT += -D__KERNEL__ -DDDE_LINUX \
          -D"KBUILD_STR(s)=\#s" \
          -D"KBUILD_BASENAME=KBUILD_STR($(patsubst %.o, %, $(notdir $@)))" \
          -D"KBUILD_MODNAME=KBUILD_STR($(patsubst %.o, %, $@))" \
          -include linux/autoconf.h

# we directly map 'jiffies' to 'dde_kit_timer_ticks'
CC_OPT += -Djiffies=dde_kit_timer_ticks

CC_WARN = -Wall -Wstrict-prototypes -fno-strict-aliasing -Wno-unused \
          -Wno-pointer-sign -Wno-unused-but-set-variable -Wno-return-type \
          -Wno-address
