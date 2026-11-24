#!/bin/bash
# ================================================================
#  PiCam fallback WiFi — the Pi makes its own network when your
#  phone hotspot isn't around
# ================================================================
#  At boot the Pi joins your usual WiFi (e.g. your phone hotspot)
#  like it always has. If none of its saved networks shows up within
#  ~45s, it starts its own access point instead:
#
#     SSID: PiCam        Pi address: 10.42.0.1 (fixed)
#
#  The Cardputer camera app knows both, so it finds the Pi either
#  way. While in AP mode the Pi keeps looking for your saved WiFi
#  every 2 minutes and hops back onto it when it appears.
#
#  Needs NetworkManager (Raspberry Pi OS Bookworm default).
#  Run:  ./setup-picam-ap.sh [password]      (default password: picam1234)
#  Undo: sudo systemctl disable --now picam-wifi && nmcli con delete PiCam-AP
# ================================================================
set -e

AP_SSID="PiCam"
AP_PASS="${1:-picam1234}"
AP_CON="PiCam-AP"

if [ ${#AP_PASS} -lt 8 ]; then echo "password must be 8+ characters"; exit 1; fi
command -v nmcli >/dev/null || { echo "NetworkManager (nmcli) not found. On Bullseye: sudo raspi-config > Advanced > Network Config > NetworkManager"; exit 1; }

echo "[1/3] Creating the $AP_SSID access point profile..."
sudo nmcli con delete "$AP_CON" >/dev/null 2>&1 || true
sudo nmcli con add type wifi ifname wlan0 con-name "$AP_CON" autoconnect no ssid "$AP_SSID" >/dev/null
sudo nmcli con modify "$AP_CON" \
  802-11-wireless.mode ap 802-11-wireless.band bg 802-11-wireless.channel 6 \
  ipv4.method shared ipv4.addresses 10.42.0.1/24 ipv6.method disabled \
  wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$AP_PASS"

echo "[2/3] Installing picam-wifi (client-first, AP fallback)..."
sudo tee /usr/local/bin/picam-wifi > /dev/null << 'WIFIEOF'
#!/bin/bash
# picam-wifi {watch|ap|client|status}
AP_CON="PiCam-AP"

ap_up()   { nmcli -t -f NAME con show --active | grep -qx "$AP_CON"; }
client_up() {
  nmcli -t -f NAME,TYPE,DEVICE con show --active | grep ':802-11-wireless:wlan0' | grep -vq "^$AP_CON:"
}
known_ssids() {
  for c in $(nmcli -t -f NAME,TYPE con show | awk -F: '$2=="802-11-wireless"{print $1}'); do
    [ "$c" = "$AP_CON" ] && continue
    nmcli -g 802-11-wireless.ssid con show "$c" 2>/dev/null
  done
}
known_visible() {
  local seen; seen=$(nmcli -t -f SSID dev wifi list --rescan yes 2>/dev/null)
  while read -r s; do [ -n "$s" ] && grep -qxF "$s" <<< "$seen" && return 0; done < <(known_ssids)
  return 1
}
go_ap()     { echo "starting AP $AP_CON"; nmcli con up "$AP_CON"; }
go_client() { echo "leaving AP, rejoining saved WiFi"; nmcli con down "$AP_CON" 2>/dev/null; nmcli dev wifi rescan 2>/dev/null; sleep 2; nmcli dev connect wlan0 2>/dev/null || true; }

case "$1" in
  watch)
    # boot: give the usual WiFi ~45s
    for i in $(seq 45); do client_up && break; sleep 1; done
    client_up || go_ap
    while true; do
      sleep 120
      if ap_up; then
        known_visible && { go_client; sleep 30; client_up || go_ap; }
      elif ! client_up; then
        sleep 30; client_up || go_ap
      fi
    done
    ;;
  ap) go_ap ;;
  client) go_client; sleep 20; client_up || { echo "no saved WiFi found, back to AP"; go_ap; exit 1; } ;;
  status)
    if ap_up; then echo "ap PiCam 10.42.0.1"
    elif client_up; then echo "client $(nmcli -t -f GENERAL.CONNECTION dev show wlan0 | cut -d: -f2) $(hostname -I | awk '{print $1}')"
    else echo "down"; fi
    ;;
  *) echo "usage: picam-wifi {watch|ap|client|status}"; exit 1 ;;
esac
WIFIEOF
sudo chmod 0755 /usr/local/bin/picam-wifi

echo "[3/3] systemd service..."
sudo tee /etc/systemd/system/picam-wifi.service > /dev/null << 'SVCEOF'
[Unit]
Description=PiCam WiFi: saved networks first, own AP as fallback
After=NetworkManager.service
Wants=NetworkManager.service

[Service]
Type=simple
ExecStart=/usr/local/bin/picam-wifi watch
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
SVCEOF
sudo systemctl daemon-reload
sudo systemctl enable --now picam-wifi >/dev/null 2>&1

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║   📶 PiCam fallback WiFi installed                ║"
echo "╠══════════════════════════════════════════════════╣"
echo "  Hotspot around  -> Pi joins it as usual"
echo "  No hotspot      -> Pi hosts:  SSID $AP_SSID"
echo "                               pass $AP_PASS"
echo "                               Pi at 10.42.0.1"
echo ""
echo "  Force it:  sudo picam-wifi ap | sudo picam-wifi client"
echo "  State:     picam-wifi status"
echo "╚══════════════════════════════════════════════════╝"
