pic16_ss = ss.source_set()
pic16_ss.add(files('boot.c'))
pic16_ss.add(when: 'CONFIG_PIC16_SOC', if_true: files(
  'pic16f1_soc.c',
  'pic16_port.c',
  'pic16_eusart.c',
  'pic16_mssp.c',
  'pic16_tmr1.c',
  'pic16_wwdt.c'))
pic16_ss.add(when: 'CONFIG_PIC16F17546', if_true: files('pic16f17546.c'))
pic16_ss.add(when: 'CONFIG_SANDCASTLE', if_true: files(
  'sandcastle.c',
  'mcp23s08.c',
  'pic16_sim_bridge.c'))
pic16_ss.add(when: 'CONFIG_PIC16_TEST', if_true: files('pic16_test.c'))

hw_arch += {'pic16': pic16_ss}
