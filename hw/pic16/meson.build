pic16_ss = ss.source_set()
pic16_ss.add(files('boot.c'))
pic16_ss.add(when: 'CONFIG_PIC16_TEST', if_true: files('pic16_test.c'))

hw_arch += {'pic16': pic16_ss}
