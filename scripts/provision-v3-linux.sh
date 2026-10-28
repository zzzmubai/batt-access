#!/bin/sh
# This installer is deliberately restricted to our new NoCloud M0 VM.
set -eu
[ "$(id -u)" = 0 ] || { echo 'Run with sudo on the new V3 VM.' >&2; exit 2; }
. /etc/os-release
[ "$ID" = ubuntu ] && [ "$VERSION_ID" = 24.04 ] || { echo 'Ubuntu 24.04 required.' >&2; exit 2; }
[ "$(cat /var/lib/cloud/data/instance-id)" = batt-v3-m0-20261028 ] || { echo 'Not the dedicated V3 VM.' >&2; exit 2; }
[ "$(hostname)" = batt-v3-mysql ] || { echo 'Unexpected hostname.' >&2; exit 2; }
if command -v mysql_config >/dev/null 2>&1; then
  case "$(mysql_config --version)" in 8.4.*) ;; *) echo 'Foreign existing MySQL client; refusing replacement.' >&2; exit 2 ;; esac
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential cmake ninja-build pkg-config python3-venv ca-certificates curl gnupg libssl-dev zlib1g-dev libzstd-dev
install -d -m 0755 /usr/share/keyrings
curl --fail --location --max-time 60 https://repo.mysql.com/RPM-GPG-KEY-mysql-2025 -o /tmp/batt-v3-mysql-key.asc
gpg --batch --yes --dearmor -o /usr/share/keyrings/batt-v3-mysql.gpg /tmp/batt-v3-mysql-key.asc
gpg --show-keys --with-fingerprint /usr/share/keyrings/batt-v3-mysql.gpg
printf '%s\n' 'deb [arch=amd64 signed-by=/usr/share/keyrings/batt-v3-mysql.gpg] https://repo.mysql.com/apt/ubuntu/ noble mysql-8.4-lts' > /etc/apt/sources.list.d/batt-v3-mysql.list
apt-get update
apt-cache policy mysql-community-server libmysqlclient-dev
for package in mysql-community-server libmysqlclient-dev; do
  candidate=$(apt-cache policy "$package" | awk '/Candidate:/ { print $2 }')
  case "$candidate" in 8.4.*) ;; *) echo 'MySQL 8.4 package candidate required.' >&2; exit 2 ;; esac
done
apt-get install -y mysql-community-server libmysqlclient-dev
printf '%s\n' '[mysqld]' 'bind-address=127.0.0.1' 'mysqlx-bind-address=127.0.0.1' 'innodb_flush_log_at_trx_commit=1' 'sync_binlog=1' > /etc/mysql/mysql.conf.d/batt-v3-local.cnf
systemctl restart mysql
mysql_config --version
mysql --protocol=socket --user=root --batch --skip-column-names -e 'SELECT VERSION(),@@innodb_flush_log_at_trx_commit,@@log_bin,@@sync_binlog'
