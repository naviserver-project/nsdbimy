#
# nsdbimy configuration example.
#
#     The nsdbimy MySQL database driver accepts 6
#     extra configuration parmeters:
#
#     databse:    (default "mysql")
#     user:       (default "root")
#     password:   (default blank)
#     host:       (mysql default)
#     port:       (mysql default)
#     unixdomain: (mysql default)
#


#
# Global pools.
#
ns_section "ns/modules"
ns_param   pool1          $bindir/nsdbimy.so


#
# Private pools
#
ns_section "ns/server/server1/modules"
ns_param   pool2          $bindir/nsdbimy.so


#
# Pool 2 configuration.
#
ns_section "ns/server/server1/module/pool2"
#
# The following are standard nsdbi config options.
# See nsdbi.n for details.
#
ns_param   default        true ;# This is the default pool for server1.
ns_param   handles        2    ;# Max open handles to db.
ns_param   maxwiat        10   ;# Seconds to wait if handle unavailable.
ns_param   maxidle        0    ;# Handle closed after maxidle seconds if unused.
ns_param   maxopen        0    ;# Handle closed after maxopen seconds, regardles of use.
ns_param   maxqueries     0    ;# Handle closed after maxqueries sql queries.
ns_param   checkinterval  600  ;# Check for idle handles every 10 minutes.
#
# Following is the mysql connection info that specifies
# which database to connect to, user name, etc.
#
ns_param   database       "mysql"
ns_param   user           "root"
#ns_param   password       xxx
#ns_param   host           localhost
#ns_param   port           3306
#ns_param   unixdomain     /var/lib/mysql/mysql.sock
