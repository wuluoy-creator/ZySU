#!/bin/sh
# Run with: sh userspace/ksud/tests/installer_zip_error_test.sh

export BOOTMODE=true
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
work_dir=$(mktemp -d) || exit 1
trap 'rm -rf "$work_dir"' 0
# shellcheck source=/dev/null
. "$script_dir/../assets/installer.sh"

# Keep the production install_module and abort functions; replace device-only
# operations and inject unzip results at each required extraction step.
chcon() { :; }
mount_partitions() { :; }
api_level_arch_detect() { :; }
check_managed_features() { :; }
set_perm_recursive() { :; }
grep_prop() { sed -n "s/^$1=//p" "$2" | head -n 1; }
unzip() {
  case "$1:$3" in
    -l:*)
      case "$mock_case" in legacy_*) printf 'install.sh\n';; esac
      return 0
      ;;
    -o:module.prop)
      printf 'id=test\nname=Test\nauthor=Test\n' > "$TMPDIR/module.prop"
      [ "$mock_case" = metadata_failure ] && return 2
      return 0
      ;;
    -oj:module.prop)
      [ "$mock_case" = legacy_failure ] && return 2
      printf 'print_modname() { :; }\non_install() { :; }\nset_permissions() { :; }\n' > "$TMPDIR/install.sh"
      return 0
      ;;
    -o:customize.sh)
      return 0
      ;;
    -o:-x)
      [ "$mock_case" = files_failure ] && return 2
      cp "$TMPDIR/module.prop" "$MODPATH/module.prop"
      return 0
      ;;
  esac
  printf 'Unexpected unzip call: %s\n' "$*" >&2
  return 2
}

run_case() {
  mock_case=$1
  expected_status=$2
  case_dir="$work_dir/$mock_case"
  mkdir -p "$case_dir"
  if (
    TMPDIR="$case_dir/tmp"
    export NVBASE="$case_dir/data"
    export ZIPFILE="$case_dir/module.zip"
    MODPATH=''
    export SKIPMOUNT=false
    export PROPFILE=false
    export POSTFSDATA=false
    export LATESTARTSERVICE=false
    install_module
  ) > "$case_dir/output" 2>&1; then
    actual_status=0
  else
    actual_status=$?
  fi

  if [ "$actual_status" -ne "$expected_status" ]; then
    printf '%s: expected exit %s, got %s\n' "$mock_case" "$expected_status" "$actual_status" >&2
    cat "$case_dir/output" >&2
    exit 1
  fi
  if [ "$expected_status" -eq 0 ]; then
    grep -q '^- Done$' "$case_dir/output" || exit 1
    [ -f "$case_dir/data/modules/test/module.prop" ] || exit 1
  else
    grep -q '^! Unable to extract zip file!$' "$case_dir/output" || exit 1
    if grep -q '^- Done$' "$case_dir/output"; then
      printf '%s: reported success after unzip failed\n' "$mock_case" >&2
      exit 1
    fi
    [ ! -e "$case_dir/data/modules_update/test" ] || exit 1
  fi
}

run_case metadata_failure 1
run_case legacy_failure 1
run_case files_failure 1
run_case legacy_success 0
run_case success 0
printf 'Installer ZIP error tests passed\n'
