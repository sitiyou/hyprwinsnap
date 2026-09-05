project('hyprwinsnap', 'cpp',
    version: '1.0',
    license: 'MIT',
    default_options: ['cpp_std=c++23', 'warning_level=2', 'optimization=2', 'b_lundef=false'],
)

# hyprland headers are not warning-clean; treat them as system includes
cflags = run_command('pkg-config', '--cflags', 'hyprland', check: true, capture: true).stdout().strip()
cpp_args = []
foreach flag : cflags.split(' ')
    if flag.startswith('-I')
        cpp_args += ['-isystem', flag.substring(2)]
    else
        cpp_args += flag
    endif
endforeach

shared_library('hyprwinsnap',
    'src/main.cpp',
    cpp_args: cpp_args,
    name_prefix: '',
    install: true,
    install_dir: get_option('libdir') / 'hyprland',
)