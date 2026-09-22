pic18_ss = ss.source_set()
pic18_ss.add(files('boot.c'))
pic18_ss.add(when: 'CONFIG_PIC18_SOC', if_true: files(
  'pic18q43_soc.c',
  'pic18_port.c',
  'pic18_spi.c',
  'pic18_tmr0.c',
  'pic18_wwdt.c'))
# The simulation bridge is target-neutral and shared with the PIC16 boards.
pic18_ss.add(when: 'CONFIG_PIC18_DEVBOARD', if_true: files(
  'pic18_devboard.c',
  'pic18_cosim_link.c',
  '../pic16/pic16_sim_bridge.c'))

pic18_ss.add(when: 'CONFIG_PIC18_TEST', if_true: files('pic18_test.c'))

hw_arch += {'pic18': pic18_ss}
