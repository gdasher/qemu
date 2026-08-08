mips_ss.add(when: 'CONFIG_PIC32MK_SOC', if_true: files(
  'pic32mk_soc.c',
  'pic32_regs.c',
  'pic32_cru.c',
  'pic32_pps.c',
  'pic32_gpio.c',
  'pic32_uart.c',
  'pic32_spi.c',
  'boot.c'))
mips_ss.add(when: 'CONFIG_PIC32_DEVBOARD', if_true: files('pic32_devboard.c'))
