#!/bin/sh
USER=$(/usr/sbin/nvram get pptpd_client_srvuser)
PASS=$(/usr/sbin/nvram get pptpd_client_srvpass)
SEC=$(/usr/sbin/nvram get pptpd_client_srvsec)
MTU=$(/usr/sbin/nvram get pptpd_client_srvmtu)
MRU=$(/usr/sbin/nvram get pptpd_client_srvmru)
OPTIONS=$(/usr/sbin/nvram get pptpd_client_options)

sleep 5
rm -rf /tmp/pptpd_client
mkdir /tmp/pptpd_client
mkdir /tmp/ppp
if [ -z "$OPTIONS" ]; then
   cp /etc/config/pptpd_client.options /tmp/pptpd_client/options.vpn
else
   echo "$OPTIONS" >> /tmp/pptpd_client/options.vpn
fi
echo "$SEC" >> /tmp/pptpd_client/options.vpn
echo -n "mtu " >> /tmp/pptpd_client/options.vpn
echo "$MTU"  >> /tmp/pptpd_client/options.vpn
echo -n "mru " >> /tmp/pptpd_client/options.vpn
echo "$MRU" >> /tmp/pptpd_client/options.vpn
echo -n "user " >> /tmp/pptpd_client/options.vpn
echo "$USER" >> /tmp/pptpd_client/options.vpn
echo -n "password " >> /tmp/pptpd_client/options.vpn
echo "$PASS" >> /tmp/pptpd_client/options.vpn
# for routing, nat firewall rules in IP-scripts
echo "ip-up-script /tmp/pptpd_client/ip-up" >> /tmp/pptpd_client/options.vpn
echo "ip-down-script /tmp/pptpd_client/ip-down" >> /tmp/pptpd_client/options.vpn
echo "ipparam kelokepptpd" >> /tmp/pptpd_client/options.vpn
# fix line endings
sed -i s/\\r//g /tmp/pptpd_client/options.vpn
# copy files and set permissions
cp /etc/config/pptpd_client.vpn /tmp/pptpd_client/vpnc
chmod +x /tmp/pptpd_client/vpnc
cp /etc/config/pptpd_client.ip-up /tmp/pptpd_client/ip-up
chmod +x /tmp/pptpd_client/ip-up
cp /etc/config/pptpd_client.ip-down /tmp/pptpd_client/ip-down
chmod +x /tmp/pptpd_client/ip-down
# start service
pidof vpnc || /tmp/pptpd_client/vpnc start
