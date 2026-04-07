#! /usr/bin/env python3
import os
import sys
import subprocess
import shutil

try:
    import argparse as ap
except ImportError:
    import pyne._argparse as ap


def absexpanduser(x):
    return os.path.abspath(os.path.expanduser(x))


def default_prefix():
    return os.environ.get("CONDA_PREFIX", sys.prefix)


def check_windows_cmake(cmake_cmd):
    if os.name != 'nt':
        return cmake_cmd

    files_on_path = set()
    for p in os.environ.get('PATH', '').split(';')[::-1]:
        if os.path.exists(p):
            try:
                files_on_path.update(os.listdir(p))
            except OSError:
                pass

    if 'cl.exe' in files_on_path:
        pass
    elif 'sh.exe' in files_on_path:
        cmake_cmd += ['-G', 'MSYS Makefiles']
    elif 'gcc.exe' in files_on_path:
        cmake_cmd += ['-G', 'MinGW Makefiles']

    return cmake_cmd


def install(args):
    if os.path.exists(args.build_dir) and args.clean_build:
        shutil.rmtree(args.build_dir)

    if not os.path.exists(args.build_dir):
        os.mkdir(args.build_dir)

    root_dir = os.path.abspath(os.path.dirname(__file__))

    # Always re-run cmake unless build_only is intended to skip reconfigure.
    cmake_cmd = ['cmake', root_dir]

    if args.prefix:
        cmake_cmd += ['-DCMAKE_INSTALL_PREFIX=' + absexpanduser(args.prefix)]

    if args.cmake_prefix_path:
        cmake_cmd += ['-DCMAKE_PREFIX_PATH=' + absexpanduser(args.cmake_prefix_path)]

    if args.coin_root:
        cmake_cmd += ['-DCOIN_ROOT_DIR=' + absexpanduser(args.coin_root)]

    if args.cyclus_root:
        cmake_cmd += ['-DCYCLUS_ROOT_DIR=' + absexpanduser(args.cyclus_root)]

    if args.boost_root:
        cmake_cmd += ['-DBOOST_ROOT=' + absexpanduser(args.boost_root)]

    if args.build_type:
        cmake_cmd += ['-DCMAKE_BUILD_TYPE=' + args.build_type]

    cmake_cmd = check_windows_cmake(cmake_cmd)

    subprocess.check_call(cmake_cmd, cwd=args.build_dir,
                          shell=(os.name == 'nt'))

    build_cmd = ['make']
    if args.threads:
        build_cmd += ['-j' + str(args.threads)]

    subprocess.check_call(build_cmd, cwd=args.build_dir,
                          shell=(os.name == 'nt'))

    if args.test:
        test_cmd = ['make', 'test']
        subprocess.check_call(test_cmd, cwd=args.build_dir,
                              shell=(os.name == 'nt'))
    elif not args.build_only:
        install_cmd = ['make', 'install']
        subprocess.check_call(install_cmd, cwd=args.build_dir,
                              shell=(os.name == 'nt'))


def uninstall(args):
    makefile = os.path.join(args.build_dir, 'Makefile')
    if not os.path.exists(args.build_dir) or not os.path.exists(makefile):
        sys.exit("May not uninstall because it has not yet been built.")

    subprocess.check_call(['make', 'uninstall'], cwd=args.build_dir,
                          shell=(os.name == 'nt'))


def main():
    default_install = default_prefix()

    description = (
        "An installation helper script for building and installing this Cyclus "
        "archetype module."
    )
    parser = ap.ArgumentParser(description=description)

    parser.add_argument('--build_dir', default='build',
                        help='where to place the build directory')

    parser.add_argument('--uninstall', action='store_true', default=False,
                        help='uninstall')

    parser.add_argument('--clean-build', action='store_true',
                        help='remove the build directory before building')

    parser.add_argument('-j', '--threads', type=int,
                        help='number of threads to use in make')

    parser.add_argument('--prefix', default=default_install,
                        help='installation prefix (default: CONDA_PREFIX or sys.prefix)')

    parser.add_argument('--build-only', action='store_true',
                        help='only build the package, do not install')

    parser.add_argument('--test', action='store_true',
                        help='run tests after building')

    parser.add_argument('--coin_root',
                        help='path to the Coin-OR libraries directory')

    parser.add_argument('--cyclus_root',
                        help='path to Cyclus installation directory')

    parser.add_argument('--boost_root',
                        help='path to Boost libraries directory')

    parser.add_argument('--cmake_prefix_path',
                        help='CMAKE_PREFIX_PATH for find_package/find_library')

    parser.add_argument('--build_type',
                        help='CMAKE_BUILD_TYPE')

    args = parser.parse_args()

    if args.uninstall:
        uninstall(args)
    else:
        install(args)


if __name__ == "__main__":
    main()