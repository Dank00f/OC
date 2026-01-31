#!/usr/bin/env bash
set -euo pipefail

REPO="$HOME/OC_git"
LAB5="$REPO/labs/lab5"
LAB6="$REPO/labs/lab6"
KIOSK_USER="kiosk"
INSTALL_DIR="/opt/oc"
LAB5_DIR="$INSTALL_DIR/lab5"
LAB6_DIR="$INSTALL_DIR/lab6"
BIND="127.0.0.1"
PORT="8080"

[[ $EUID -eq 0 ]] || { echo "Run: sudo $0"; exit 1; }

echo "[1/7] user $KIOSK_USER"
if ! id -u "$KIOSK_USER" >/dev/null 2>&1; then
  useradd -m -s /bin/bash "$KIOSK_USER"
fi

echo "[2/7] install binaries to /opt/oc"
mkdir -p "$LAB5_DIR" "$LAB6_DIR"
[[ -x "$LAB5/build/temp_logger" ]] || { echo "FATAL: missing $LAB5/build/temp_logger (build lab5)"; exit 1; }
[[ -x "$LAB6/build/temp_gui"    ]] || { echo "FATAL: missing $LAB6/build/temp_gui (build lab6)"; exit 1; }

install -m 755 "$LAB5/build/temp_logger" "$LAB5_DIR/temp_logger"
install -m 755 "$LAB6/build/temp_gui"    "$LAB6_DIR/temp_gui"

if [[ -d "$LAB5/web" ]]; then
  rm -rf "$LAB5_DIR/web"
  cp -a "$LAB5/web" "$LAB5_DIR/web"
fi

touch "$LAB5_DIR/temp.db"
chown -R "$KIOSK_USER:$KIOSK_USER" "$INSTALL_DIR"

echo "[3/7] systemd service for lab5 server (oc-temp-logger.service)"
cat > /etc/systemd/system/oc-temp-logger.service <<EOF
[Unit]
Description=OC Lab5 Temp Logger/Server (kiosk)
After=network.target

[Service]
Type=simple
User=$KIOSK_USER
WorkingDirectory=$LAB5_DIR
ExecStart=$LAB5_DIR/temp_logger --db $LAB5_DIR/temp.db --serve --bind $BIND --port $PORT --simulate --web-dir $LAB5_DIR/web
Restart=always
RestartSec=1

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now oc-temp-logger.service

echo "[4/7] lightdm autologin to $KIOSK_USER"
LIGHTDM_CONF="/etc/lightdm/lightdm.conf"
cp -a "$LIGHTDM_CONF" "$LIGHTDM_CONF.bak.$(date +%s)" 2>/dev/null || true
grep -q "^\[Seat:\*\]" "$LIGHTDM_CONF" 2>/dev/null || printf "\n[Seat:*]\n" >> "$LIGHTDM_CONF"

if grep -q "^\s*autologin-user=" "$LIGHTDM_CONF"; then
  sed -i "s/^\s*autologin-user=.*/autologin-user=$KIOSK_USER/" "$LIGHTDM_CONF"
else
  sed -i "/^\[Seat:\*\]/a autologin-user=$KIOSK_USER" "$LIGHTDM_CONF"
fi

if grep -q "^\s*autologin-user-timeout=" "$LIGHTDM_CONF"; then
  sed -i "s/^\s*autologin-user-timeout=.*/autologin-user-timeout=0/" "$LIGHTDM_CONF"
else
  sed -i "/^\[Seat:\*\]/a autologin-user-timeout=0" "$LIGHTDM_CONF"
fi

echo "[5/7] Xsession for kiosk user (start GUI only)"
KHOME="$(eval echo ~${KIOSK_USER})"
cat > "$KHOME/.xsession" <<EOF
#!/bin/sh
xset s off
xset -dpms
xset s noblank

for i in 1 2 3 4 5 6 7 8 9 10; do
  curl -sS http://$BIND:$PORT/api/current >/dev/null 2>&1 && break
  sleep 0.3
done

exec $LAB6_DIR/temp_gui --base-url http://$BIND:$PORT
EOF
chmod +x "$KHOME/.xsession"
chown "$KIOSK_USER:$KIOSK_USER" "$KHOME/.xsession"

echo "[6/7] lock down tty + ctrl-alt-del"
systemctl mask ctrl-alt-del.target >/dev/null 2>&1 || true
for n in 2 3 4 5 6; do
  systemctl mask "getty@tty${n}.service" >/dev/null 2>&1 || true
done

echo "[7/7] done. reboot"
echo "sudo reboot"
