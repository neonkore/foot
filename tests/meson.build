config_test = executable(
  'test-config',
  'test-config.c', '../tokenize.c',
  wl_proto_headers,
  link_with: [common],
  dependencies: [pixman, xkb, fontconfig, fcft, tllist])

test('config', config_test)

if check.found()
  foot_test = executable(
    'test-foot',
    'test-foot.c',
    '../config.c', '../config.h',
    '../tokenize.c', '../tokenize.h',
    '../user-notification.c', '../user-notification.h',
    wl_proto_headers,
    dependencies: [check, pixman, xkb, fcft],
    link_with: [common]
  )
  test('foot', foot_test)
endif
