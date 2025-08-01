\newpage

# Configuration

## pgmoneta.conf

The configuration is loaded from either the path specified by the `-c` flag or `/etc/pgmoneta/pgmoneta.conf`.

The configuration of `pgmoneta` is split into sections using the `[` and `]` characters.

The main section, called `[pgmoneta]`, is where you configure the overall properties
of `pgmoneta`.

Other sections doesn't have any requirements to their naming so you can give them
meaningful names like `[primary]` for the primary [PostgreSQL](https://www.postgresql.org)
instance.

All properties are in the format `key = value`.

The characters `#` and `;` can be used for comments; must be the first character on the line.
The `Bool` data type supports the following values: `on`, `yes`, `1`, `true`, `off`, `no`, `0` and `false`.

See a [sample](./etc/pgmoneta.conf) configuration for running `pgmoneta` on `localhost`.

Note, that PostgreSQL 13+ is required, as well as having `wal_level` at `replica` or `logical` level.

### pgmoneta

#### General

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| host | | String | Yes | The bind address for pgmoneta |
| unix_socket_dir | | String | Yes | The Unix Domain Socket location |
| base_dir | | String | Yes | The base directory for the backup |

Note, that if `host` starts with a `/` it represents a path and `pgmoneta` will connect using a Unix Domain Socket.

#### Monitoring

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| metrics | 0 | Int | No | The metrics port (disable = 0) |
| metrics_cache_max_age | 0 | String | No | The time to keep a Prometheus (metrics) response in cache. If this value is specified without units, it is taken as seconds. Setting this parameter to 0 disables caching. It supports the following units as suffixes: 'S' for seconds (default), 'M' for minutes, 'H' for hours, 'D' for days, and 'W' for weeks. |
| metrics_cache_max_size | 256k | String | No | The maximum amount of data to keep in cache when serving Prometheus responses. Changes require restart. This parameter determines the size of memory allocated for the cache even if `metrics_cache_max_age` or `metrics` are disabled. Its value, however, is taken into account only if `metrics_cache_max_age` is set to a non-zero value. Supports suffixes: 'B' (bytes), the default if omitted, 'K' or 'KB' (kilobytes), 'M' or 'MB' (megabytes), 'G' or 'GB' (gigabytes).|
| metrics_cert_file | | String | No | Certificate file for TLS for Prometheus metrics. This file must be owned by either the user running pgmoneta or root. |
| metrics_key_file | | String | No | Private key file for TLS for Prometheus metrics. This file must be owned by either the user running pgmoneta or root. Additionally permissions must be at least `0640` when owned by root or `0600` otherwise. |
| metrics_ca_file | | String | No | Certificate Authority (CA) file for TLS for Prometheus metrics. This file must be owned by either the user running pgmoneta or root.  |

#### Management

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| management | 0 | Int | No | The remote management port (disable = 0) |

#### Compression

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| compression | zstd | String | No | The compression type (none, gzip, client-gzip, server-gzip, zstd, client-zstd, server-zstd, lz4, client-lz4, server-lz4, bzip2, client-bzip2) |
| compression_level | 3 | Int | No | The compression level |

#### Workers

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| workers | 0 | Int | No | The number of workers that each process can use for its work. Use 0 to disable. Maximum is CPU count |

#### Workspace

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| workspace | /tmp/pgmoneta-workspace/ | String | No | The directory for the workspace that incremental backup can use for its work |

#### Storage

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| storage_engine | local | String | No | The storage engine type (local, ssh, s3, azure) |

#### Encryption

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| encryption | none | String | No | The encryption mode for encrypt wal and data<br/> `none`: No encryption <br/> `aes \| aes-256 \| aes-256-cbc`: AES CBC (Cipher Block Chaining) mode with 256 bit key length<br/> `aes-192 \| aes-192-cbc`: AES CBC mode with 192 bit key length<br/> `aes-128 \| aes-128-cbc`: AES CBC mode with 128 bit key length<br/> `aes-256-ctr`: AES CTR (Counter) mode with 256 bit key length<br/> `aes-192-ctr`: AES CTR mode with 192 bit key length<br/> `aes-128-ctr`: AES CTR mode with 128 bit key length |

#### Slot management

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| create_slot | no | Bool | No | Create a replication slot for all server. Valid values are: yes, no |

#### SSH

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| ssh_hostname | | String | Yes | Defines the hostname of the remote system for connection |
| ssh_username | | String | Yes | Defines the username of the remote system for connection |
| ssh_base_dir | | String | Yes | The base directory for the remote backup |
| ssh_ciphers | aes-256-ctr, aes-192-ctr, aes-128-ctr | String | No | The supported ciphers for communication. `aes \| aes-256 \| aes-256-cbc`: AES CBC (Cipher Block Chaining) mode with 256 bit key length<br/> `aes-192 \| aes-192-cbc`: AES CBC mode with 192 bit key length<br/> `aes-128 \| aes-128-cbc`: AES CBC mode with 128 bit key length<br/> `aes-256-ctr`: AES CTR (Counter) mode with 256 bit key length<br/> `aes-192-ctr`: AES CTR mode with 192 bit key length<br/> `aes-128-ctr`: AES CTR mode with 128 bit key length. Otherwise verbatim |

#### S3

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| s3_aws_region | | String | Yes | The AWS region |
| s3_access_key_id | | String | Yes | The IAM access key ID |
| s3_secret_access_key | | String | Yes | The IAM secret access key |
| s3_bucket | | String | Yes | The AWS S3 bucket name |
| s3_base_dir | | String | Yes | The base directory for the S3 bucket |

#### Azure

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| azure_storage_account | | String | Yes | The Azure storage account name |
| azure_container | | String | Yes | The Azure container name |
| azure_shared_key | | String | Yes | The Azure storage account key |
| azure_base_dir | | String | Yes | The base directory for the Azure container |

#### Retention

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| retention | 7, - , - , - | Array | No | The retention time in days, weeks, months, years |

#### Verification

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| verification | 0 | String | No | The time between verification of a backup. If this value is specified without units,
  it is taken as seconds. Setting this parameter to 0 disables verification. It supports the
  following units as suffixes: 'S' for seconds (default), 'M' for minutes, 'H' for hours, 'D'
  for days, and 'W' for weeks. Default is 0 (disabled) |

#### Logging

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| log_type | console | String | No | The logging type (console, file, syslog) |
| log_level | info | String | No | The logging level, any of the (case insensitive) strings `FATAL`, `ERROR`, `WARN`, `INFO` and `DEBUG` (that can be more specific as `DEBUG1` thru `DEBUG5`). Debug level greater than 5 will be set to `DEBUG5`. Not recognized values will make the log_level be `INFO` |
| log_path | pgmoneta.log | String | No | The log file location. Can be a strftime(3) compatible string. |
| log_rotation_age | 0 | String | No | The time after which log file rotation is triggered. If this value is specified without units, it is taken as seconds. Setting this parameter to 0 disables log rotation based on time. It supports the following units as suffixes: 'S' for seconds (default), 'M' for minutes, 'H' for hours, 'D' for days, and 'W' for weeks. |
| log_rotation_size | 0 | String | No | The size of the log file that will trigger a log rotation. Supports suffixes: 'B' (bytes), the default if omitted, 'K' or 'KB' (kilobytes), 'M' or 'MB' (megabytes), 'G' or 'GB' (gigabytes). A value of `0` (with or without suffix) disables. |
| log_line_prefix | %Y-%m-%d %H:%M:%S | String | No | A strftime(3) compatible string to use as prefix for every log line. Must be quoted if contains spaces. |
| log_mode | append | String | No | Append to or create the log file (append, create) |

#### Transport Level Security

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| tls | `off` | Bool | No | Enable Transport Layer Security (TLS) |
| tls_cert_file | | String | No | Certificate file for TLS. This file must be owned by either the user running pgmoneta or root. |
| tls_key_file | | String | No | Private key file for TLS. This file must be owned by either the user running pgmoneta or root. Additionally permissions must be at least `0640` when owned by root or `0600` otherwise. |
| tls_ca_file | | String | No | Certificate Authority (CA) file for TLS. This file must be owned by either the user running pgmoneta or root.  |
| libev | `auto` | String | No | Select the [libev](http://software.schmorp.de/pkg/libev.html) backend to use. Valid options: `auto`, `select`, `poll`, `epoll`, `iouring`, `devpoll` and `port` |

#### Miscellaneous

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| backup_max_rate | 0 | Int | No | The number of bytes of tokens added every one second to limit the backup rate|
| network_max_rate | 0 | Int | No | The number of bytes of tokens added every one second to limit the netowrk backup rate|
| blocking_timeout | 30 | String | No | The number of seconds the process will be blocking for a connection. If this value is specified without units, it is taken as seconds. Setting this parameter to 0 disables it. It supports the following units as suffixes: 'S' for seconds (default), 'M' for minutes, 'H' for hours, 'D' for days, and 'W' for weeks. |
| keep_alive | on | Bool | No | Have `SO_KEEPALIVE` on sockets |
| nodelay | on | Bool | No | Have `TCP_NODELAY` on sockets |
| non_blocking | on | Bool | No | Have `O_NONBLOCK` on sockets |
| backlog | 16 | Int | No | The backlog for `listen()`. Minimum `16` |
| hugepage | `try` | String | No | Huge page support (`off`, `try`, `on`) |
| pidfile | | String | No | Path to the PID file. If not specified, it will be automatically set to `unix_socket_dir/pgmoneta.<host>.pid` where `<host>` is the value of the `host` parameter or `all` if `host = *`.|
| update_process_title | `verbose` | String | No | The behavior for updating the operating system process title. Allowed settings are: `never` (or `off`), does not update the process title; `strict` to set the process title without overriding the existing initial process title length; `minimal` to set the process title to the base description; `verbose` (or `full`) to set the process title to the full description. Please note that `strict` and `minimal` are honored only on those systems that do not provide a native way to set the process title (e.g., Linux). On other systems, there is no difference between `strict` and `minimal` and the assumed behaviour is `minimal` even if `strict` is used. `never` and `verbose` are always honored, on every system. On Linux systems the process title is always trimmed to 255 characters, while on system that provide a natve way to set the process title it can be longer. |

### Server section

#### Server

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| host | | String | Yes | The address of the PostgreSQL instance |
| port | | Int | Yes | The port of the PostgreSQL instance |
| user | | String | Yes | The replication user name |
| wal_slot | | String | Yes | The replication slot for WAL |

The `user` specified must have the `REPLICATION` option in order to stream the Write-Ahead Log (WAL), and must
have access to the `postgres` database in order to get the necessary configuration parameters.

#### Slot management

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| create_slot | no | Bool | No | Create a replication slot for this server. Valid values are: yes, no |

#### Follow

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| follow | | String | No | Failover to this server if follow server fails |

#### Retention

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| retention | | Array | No | The retention for the server in days, weeks, months, years |

#### WAL shipping

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| wal_shipping | | String | No | The WAL shipping directory |

#### Hot standby

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| hot_standby | | String | No | Hot standby directory. Single directory or comma separated directories up to 8 (e.g., /path/to/hot/standby1,/path/to/hot/standby2) |
| hot_standby_overrides | | String | No | Files to override in the hot standby directory. If multiple hot standbys are specified then this setting is separated by a \| |
| hot_standby_tablespaces | | String | No | Tablespace mappings for the hot standby. Syntax is [from -> to,?]+. If multiple hot standbys are specified then this setting is separated by a \| |

#### Workers

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| workers | -1 | Int | No | The number of workers that each process can use for its work. Use 0 to disable, -1 means use the global settting. Maximum is CPU count |

#### Transport Level Security

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| tls_cert_file | | String | No | Certificate file for TLS. This file must be owned by either the user running pgmoneta or root. |
| tls_key_file | | String | No | Private key file for TLS. This file must be owned by either the user running pgmoneta or root. Additionally permissions must be at least `0640` when owned by root or `0600` otherwise. |
| tls_ca_file | | String | No | Certificate Authority (CA) file for TLS. This file must be owned by either the user running pgmoneta or root.  |

#### Miscellaneous

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| backup_max_rate | -1 | Int | No | The number of bytes of tokens added every one second to limit the backup rate. Use 0 to disable, -1 means use the global settting|
| network_max_rate | -1 | Int | No | The number of bytes of tokens added every one second to limit the netowrk backup rate. Use 0 to disable, -1 means use the global settting|

#### Extra

| Property | Default | Unit | Required | Description |
| :------- | :------ | :--- | :------- | :---------- |
| extra | | String | No | The source directory for retrieval on the server side (details are in the extra section)|

The `extra` configuration is set in the server section. It is not required, but if you configure this parameter, when you perform a backup using the CLI `pgmoneta-cli -c pgmoneta.conf backup primary`, it will also copy all specified files on the server side and send them back to the client side.

This `extra` feature requires the server side to install the [pgmoneta_ext](https://github.com/pgmoneta/pgmoneta_ext) extension and also make the user `repl` a `SUPERUSER` (this will be improved in the future). Currently, this feature is only available to the `SUPERUSER` role.

You can set up `pgmoneta_ext` by following the [README](https://github.com/pgmoneta/pgmoneta_ext/blob/main/README.md) to easily install the extension. There are also more detailed instructions available in the [DEVELOPERS](https://github.com/pgmoneta/pgmoneta_ext/blob/main/doc/DEVELOPERS.md) documentation.

The format for the `extra` parameter is a path to a file or directory. You can list more than one file or directory separated by commas. The format is as follows:

```ini
extra = /tmp/myfile1, /tmp/myfile2, /tmp/mydir1, /tmp/mydir2
```

## pgmoneta_users.conf

The `pgmoneta_users` configuration defines the users known to the system. This file is created and managed through
the `pgmoneta-admin` tool.

The configuration is loaded from either the path specified by the `-u` flag or `/etc/pgmoneta/pgmoneta_users.conf`.

## pgmoneta_admins.conf

The `pgmoneta_admins` configuration defines the administrators known to the system. This file is created and managed through
the `pgmoneta-admin` tool.

The configuration is loaded from either the path specified by the `-A` flag or `/etc/pgmoneta/pgmoneta_admins.conf`.

If pgmoneta has both Transport Layer Security (TLS) and `management` enabled then `pgmoneta-cli` can
connect with TLS using the files `~/.pgmoneta/pgmoneta.key` (must be 0600 permission),
`~/.pgmoneta/pgmoneta.crt` and `~/.pgmoneta/root.crt`.

## Configuration Directory

You can specify a directory for all configuration files using the `-D` flag (or `--directory`).
Alternatively, you can set the `PGMONETA_CONFIG_DIR` environment variable to define the configuration directory.

**Behavior:**
- When the directory flag (`-D`) is set, pgmoneta will look for all configuration files in the specified directory.
- If a required file is not found in the specified directory, pgmoneta will look for it in its default location (e.g., `/etc/pgmoneta/pgmoneta.conf`).
- If the file is not found in either location:
  - If the file is mandatory, pgmoneta will log an error and fail to start.
  - If the file is optional, pgmoneta will log a warning and continue without it.
- All file lookup attempts and missing files are logged for troubleshooting.

**Precedence Rules:**
- Individual file flags (such as `-c`, `-u`, `-A`, etc.) always take precedence over the directory flag and environment variable for their respective files.
- The directory flag (`-D`) takes precedence over the environment variable (`PGMONETA_CONFIG_DIR`).
- If neither the directory flag nor individual file flags are set, pgmoneta uses the default locations for all configuration files.

**Using the Environment Variable:**
1. Set the environment variable before starting pgmoneta:
```
export PGMONETA_CONFIG_DIR=/path/to/config_dir
pgmoneta -d
```
2. If both the environment variable and the `-D` flag are set, the flag takes precedence.

**Example:**
```
pgmoneta -D /custom/config/dir -d
```
or
```
export PGMONETA_CONFIG_DIR=/custom/config/dir
pgmoneta -d
```

Refer to logs for details about which configuration files were loaded and from which locations.

