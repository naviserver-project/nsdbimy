#
# nsdbimy configuration test.
#


set homedir   [pwd]
set bindir    [file dirname [ns_info nsd]]



#
# Global Naviserver parameters.
#

ns_section "ns/parameters"
ns_param   home           $homedir
ns_param   tcllibrary     $bindir/../tcl
ns_param   logdebug       false

ns_section "ns/modules"
ns_param   nssock         $bindir/nssock.so

ns_section "ns/module/nssock"
ns_param   port            8080
ns_param   hostname        localhost
ns_param   address         127.0.0.1
ns_param   defaultserver   server1

ns_section "ns/module/nssock/servers"
ns_param   server1         server1

ns_section "ns/servers"
ns_param   server1         "Server One"


#
# Server One configuration.
#

ns_section "ns/server/server1/tcl"
ns_param   initfile        ${bindir}/init.tcl
ns_param   library         $homedir/tests/testserver/modules

ns_section "ns/server/server1/modules"
ns_param   pool1           $homedir/nsdbimy.so
ns_param   pool2           $homedir/nsdbimy.so
ns_param   pool3           $homedir/nsdbimy.so

#
# Database configuration.
#

ns_section "ns/server/server1/module/pool1"
ns_param   default         true
ns_param   handles         5
ns_param   maxidle         0
ns_param   maxopen         0
ns_param   maxqueries      10000
#ns_param   maxqueries      10000000
ns_param   checkinterval   30

ns_param   user            [ns_env get -nocomplain DBIMY_USER]
ns_param   password        [ns_env get -nocomplain DBIMY_PASSWORD]
ns_param   database        test

ns_section "ns/server/server1/module/pool2"
ns_param   user            "invalid user name"

ns_section "ns/server/server1/module/pool3"
ns_param   database        "invalid database name"

