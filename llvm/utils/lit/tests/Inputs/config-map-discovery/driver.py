import lit.util
import os
import sys

main_config = sys.argv[1]

config_map = {lit.util.norm_path(main_config) : sys.argv[2]}
builtin_parameters = {'config_map' : config_map}

if __name__=='__main__':
    from lit.main import main
    main_config_dir = os.path.dirname(main_config)
    sys.argv = [sys.argv[0]] + sys.argv[3:] + [main_config_dir]
    main(builtin_parameters)
