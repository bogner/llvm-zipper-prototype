#!/bin/sh

CLOOG_HASH="cloog-0.16.3"
ISL_HASH="cd1939ed06617d00159e8e51b72a804b467e98b4"

check_command_line() {
  if [ "${1}x" = "x" ] || [ "${2}x" != "x" ]
  then
      echo "Usage: " ${0} '<Directory to checkout CLooG>'
  else
    CLOOG_DIR="${1}"
  fi
}

check_cloog_directory() {
  if not [ -e ${CLOOG_DIR} ]
  then
    echo :: Directory "'${CLOOG_DIR}'" does not exists. Trying to create it.
    if not mkdir -p "${CLOOG_DIR}"
    then exit 1
    fi
  fi

  if not [ -d ${CLOOG_DIR} ]
  then
    echo "'${CLOOG_DIR}'" is not a directory
    exit 1
  fi

  if not [ -e "${CLOOG_DIR}/.git" ]
  then
    IS_GIT=0
    echo ":: No git checkout found"
    if [ `ls -A ${CLOOG_DIR}` ]
    then
      echo but directory "'${CLOOG_DIR}'" contains files
      exit 1
    fi
  else
    echo ":: Existing git repo found"
    IS_GIT=1
  fi
}

complain() {
  echo "$@"
  exit 1
}

run() {
  $cmdPre $*
  if [ $? != 0 ]
    then
    complain $* failed
  fi
}

check_command_line $@
check_cloog_directory

cd ${CLOOG_DIR}




if [ ${IS_GIT} -eq 0 ]
then
  echo :: Performing initial checkout
  run git clone git://repo.or.cz/cloog.git .
  run git submodule init
  run git submodule update
fi

echo :: Fetch versions required by Polly
run git remote update
run git reset --hard "${CLOOG_HASH}"
run cd isl
run git remote update
run git reset --hard "${ISL_HASH}"

echo :: Generating configure
run ./autogen.sh

echo :: If you install cloog/isl the first time run "'./configure'" followed by
echo :: "'make'" and "'make install'", otherwise, just call "'make'" and
echo :: "'make'" install.
