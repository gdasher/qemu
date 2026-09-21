gen = [
  decodetree.process('insn.decode', extra_args: [ '--decode', 'decode_insn',
                                                  '--insnwidth', '16' ])
]

pic18_ss = ss.source_set()
pic18_ss.add(gen)
pic18_ss.add(files(
  'translate.c',
  'helper.c',
  'cpu.c',
  'disas.c'))

pic18_system_ss = ss.source_set()

pic18_common_system_ss = ss.source_set()
pic18_common_system_ss.add(files('machine.c'))

target_arch += {'pic18': pic18_ss}
target_system_arch += {'pic18': pic18_system_ss}
target_common_system_arch += {'pic18': pic18_common_system_ss}
