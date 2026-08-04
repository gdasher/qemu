gen = [
  decodetree.process('insn.decode', extra_args: [ '--decode', 'decode_insn',
                                                  '--insnwidth', '16' ])
]

pic16_ss = ss.source_set()
pic16_ss.add(gen)
pic16_ss.add(files(
  'translate.c',
  'helper.c',
  'cpu.c',
  'disas.c'))

pic16_system_ss = ss.source_set()

pic16_common_system_ss = ss.source_set()
pic16_common_system_ss.add(files('machine.c'))

target_arch += {'pic16': pic16_ss}
target_system_arch += {'pic16': pic16_system_ss}
target_common_system_arch += {'pic16': pic16_common_system_ss}
