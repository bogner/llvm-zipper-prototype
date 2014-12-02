#!/bin/sh

ISL_HASH="2c19ecd444095d6f560349018f68993bc0e03691"

PWD=`pwd`

check_command_line() {
  if [ $# -eq 1 ]
  then
    ISL_DIR="${1}"
  else
      echo "Usage: " ${0} '<Directory to checkout isl>'
      exit 1
  fi
}

check_isl_directory() {
  if ! [ -e ${ISL_DIR} ]
  then
    echo :: Directory "'${ISL_DIR}'" does not exists. Trying to create it.
    if ! mkdir -p "${ISL_DIR}"
    then exit 1
    fi
  fi

  if ! [ -d ${ISL_DIR} ]
  then
    echo "'${ISL_DIR}'" is not a directory
    exit 1
  fi

  # Make it absolute
  cd ${ISL_DIR}
  ISL_DIR=`pwd`

  if ! [ -e "${ISL_DIR}/.git" ]
  then
    echo ":: No git checkout found"
    IS_GIT=0
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
check_isl_directory

if [ ${IS_GIT} -eq 0 ]
then
  echo :: Performing initial checkout
  # Remove the existing CLooG and ISL dirs to avoid crashing older git versions.
  cd ${ISL_DIR}/..
  run rmdir "${ISL_DIR}"
  run git clone http://repo.or.cz/r/isl.git ${ISL_DIR}
fi

echo :: Fetch version required by Polly
run cd ${ISL_DIR}
run git remote update

echo :: Setting isl version
run cd ${ISL_DIR}
run git reset --hard "${ISL_HASH}"

echo :: Generating configure
run cd ${ISL_DIR}
run ./autogen.sh

echo :: If you install isl the first time run "'./configure'" followed by
echo :: "'make'" and "'make install'", otherwise, just call "'make'" and
echo :: "'make'" install.
