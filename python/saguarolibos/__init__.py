'''Python support for Saguaro'''
import os as _os

__version__ = '@VERSION@'

_CONFIG_PKGLIBDIR = '@PKGLIBDIR@'
_CONFIG_LIBDIR = '@LIBDIR@'
_CONFIG_SYSLIBDIR = '@SYSLIBDIR@'
_CONFIG_LIBC = '@LIBC@'  # the meson option `libc`: none, glibc or musl
_CONFIG_SGX_ENABLED = '@SGX_ENABLED@' == '1'

if __version__.startswith('@') and not _os.getenv('SAGUARO_IMPORT_FOR_SPHINX_ANYWAY') == '1':
    raise RuntimeError(
        'You are attempting to run the tools from repo, without installing. '
        'Please install Saguaro (scripts/install.sh, see INSTALL.md) before running Python '
        'tools.')

# pylint: disable=wrong-import-position
from .gen_jinja_env import make_env

_env = make_env()

from .manifest import Manifest, ManifestError
if _CONFIG_SGX_ENABLED:
    from .sgx_get_token import get_token, is_oot
    from .sgx_sign import get_tbssigstruct, sign_with_local_key, SGX_LIBPAL, SGX_RSA_KEY_PATH
    from .sigstruct import Sigstruct
