import os
import pathlib
import subprocess
import sys
import sysconfig

import jinja2

from . import _CONFIG_LIBC, _CONFIG_PKGLIBDIR

def parse_ldd(output):
    # Be careful: We have to skip vdso, which doesn't have a corresponding file on the disk (we
    # assume that such files have paths starting with '/', seems ldd always prints absolute paths).
    # Also, old ldd (from Ubuntu 16.04) prints vdso differently than newer ones:
    # old:
    #     linux-vdso.so.1 =>  (0x00007ffd31fee000)
    # new:
    #     linux-vdso.so.1 (0x00007ffd31fee000)
    ret = set()
    for line in output.rstrip('\n').split('\n'):
        line = line.strip().split()
        if len(line) == 1 and line[0].endswith(':'):
            # header for when there are multiple binaries given
            continue
        if line[1] == '=>' and line[2].startswith('/'):
            ret.add(line[2])
        elif line[0].startswith('/') and line[1].startswith('/'):
            ret.add(line[0])
    return sorted(ret)

def ldd(*args):
    '''
    Args:
        binaries for which to generate manifest trusted files list.
    '''
    return parse_ldd(
        subprocess.check_output(['ldd', *(os.fspath(i) for i in args)]).decode('ascii'))

def python_get_sys_path(interpreter, include_nonexisting=False):
    for path in subprocess.check_output([interpreter, '-c',
        '''import sys; print('\\0'.join(path for path in sys.path if path), end='')'''
    ]).split(b'\0'):
        path = pathlib.Path(os.fsdecode(path))
        if not include_nonexisting and not path.exists():
            continue
        yield path

def add_globals_from_python(env):
    paths = sysconfig.get_paths()
    env.globals['python'] = {
        'stdlib': pathlib.Path(paths['stdlib']),
        'platstdlib': pathlib.Path(paths['platstdlib']),
        'purelib': pathlib.Path(paths['purelib']),

        # TODO rpm-based distros
        'distlib': pathlib.Path(sysconfig.get_path('stdlib',
                vars={'py_version_short': sys.version_info[0]})
            ) / 'dist-packages',

        'get_config_var': sysconfig.get_config_var,
        'ext_suffix': sysconfig.get_config_var('EXT_SUFFIX'),

        'get_path': sysconfig.get_path,
        'get_paths': sysconfig.get_paths,

        'implementation': sys.implementation,

        'get_sys_path': python_get_sys_path,
    }

def host_libc_dir():
    """Directory of the host's glibc and its dynamic loader.

    Used when Gramine was built without a bundled libc (meson option `libc=none`): applications then
    run on the libc of the machine the manifest is rendered on. `SAGUARO_HOST_LIBC_DIR` overrides
    the search, for example when manifests for another machine are rendered.
    """
    override = os.environ.get('SAGUARO_HOST_LIBC_DIR')
    if override:
        return pathlib.Path(override).resolve()

    candidates = []
    multiarch = sysconfig.get_config_var('MULTIARCH')
    if multiarch:
        candidates += [f'/lib/{multiarch}', f'/usr/lib/{multiarch}']
    candidates += ['/lib64', '/usr/lib64', '/lib', '/usr/lib']

    for candidate in candidates:
        path = pathlib.Path(candidate)
        if (path / 'libc.so.6').exists() and any(path.glob('ld-linux*.so*')):
            return path.resolve()

    raise RuntimeError('This Gramine was built without a bundled libc (libc=none), but no glibc '
                       'was found in ' + ', '.join(candidates) + '. Set SAGUARO_HOST_LIBC_DIR to '
                       'the directory that holds libc.so.6 and the dynamic loader.')

class Runtimedir:
    @staticmethod
    def __call__(libc='glibc'):
        if _CONFIG_LIBC == 'none' and libc == 'glibc':
            return host_libc_dir()
        return (pathlib.Path(_CONFIG_PKGLIBDIR) / 'runtime' / libc).resolve()
    def __str__(self):
        return str(self())
    def __truediv__(self, other):
        return self() / other

def add_globals_from_saguaro(env):
    env.globals['saguaro'] = {
        'libos': pathlib.Path(_CONFIG_PKGLIBDIR) / 'libsysdb.so',
        'pkglibdir': pathlib.Path(_CONFIG_PKGLIBDIR),
        # the graphics preload library; installed only with -Dgraphics=enabled and a VM-type PAL
        'libgraphics': pathlib.Path(_CONFIG_PKGLIBDIR) / 'libgraphics.so',
        'runtimedir': Runtimedir(),
    }

    try:
        import _graminelibos_offsets as offsets # pylint: disable=import-outside-toplevel
    except ImportError: # no SGX gramine installed, skipping
        pass
    else:
        env.globals['saguaro'].update(
            (k, v) for k, v in offsets.__dict__.items()
            if not k.startswith('_'))

def add_globals_misc(env):
    env.globals['env'] = os.environ
    env.globals['ldd'] = ldd

class _Undefined(jinja2.StrictUndefined):
    """StrictUndefined, with a hint for templates that still use the old variable name."""
    def _fail_with_undefined_error(self, *args, **kwargs):
        if self._undefined_name == 'gramine' and self._undefined_obj is jinja2.utils.missing:
            raise self._undefined_exception(
                "'gramine' is undefined: the template variable is now called 'saguaro' "
                "(for example {{ saguaro.runtimedir() }}); update a template with "
                "sed -i 's/{{ *gramine\\./{{ saguaro./g' <file>")
        return super()._fail_with_undefined_error(*args, **kwargs)

def make_env():

    # Jinja's autoescape feature escapes HTML sequences but we're rendering TOML, hence explicitly
    # setting autoescape to False
    env = jinja2.Environment(undefined=_Undefined, keep_trailing_newline=True,
                             autoescape=False)
    add_globals_from_saguaro(env)
    add_globals_from_python(env)
    add_globals_misc(env)
    return env
