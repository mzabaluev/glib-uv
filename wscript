def options(opt):
    opt.load('compiler_c waf_unit_test')

def configure(cnf):
    cnf.load('compiler_c waf_unit_test')
    cnf.check_cfg(package='glib-2.0',
                  args=['--cflags', '--libs'],
                  uselib_store='GLIB')
    cnf.check_cfg(package='libuv',
                  args=['--cflags', '--libs'],
                  uselib_store='UV')

def build(bld):
    bld.shlib(source='glib-uv.c',
              target='glib-uv',
              use=['GLIB', 'UV'])
    bld.program(features='test includes',
                source='glib-uv-test.c',
                target='glib-uv-test',
                includes='.',
                use=['glib-uv', 'GLIB', 'UV'])
    from waflib.Tools import waf_unit_test
    bld.add_post_fun(waf_unit_test.summary)
