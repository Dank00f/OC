#!/usr/bin/env bash
set -euo pipefail

REPO="${HOME}/OC_git"
LAB5="${REPO}/labs/lab5"
LAB6="${REPO}/labs/lab6"
KIOSK_USER="kiosk"
PORT="8080"
BIND="127.0.0.1"

# куда ставим "прод" копии (чтобы не зависеть от гит-папки и прав)
INSTALL_DIR="/opt/oc"
LAB5_DIR="${INSTALL_DIR}/lab5"
LAB6_DIR="${INSTALL_DIR}/lab6"

need_root() { [[ "${EUID}" -eq 0 ]] || { echo "Запусти: sudo $0"; exit 1; }; }
need_root

echo "[1/9] Пакеты (lightdm/openbox/x11 tools)"
apt-get update -y
apt-get install -y lightdm openbox xterm x11-xserver-utils xdotool xbindkeys unclutter

echo "[2/9] Пользователь киоска: ${KIOSK_USER}"
if ! id -u "${KIOSK_USER}" >/dev/null 2>&1; then
  useradd -m -s /bin/bash "${KIOSK_USER}"
fi

echo "[3/9] Установка файлов lab5/lab6 в ${INSTALL_DIR}"
mkdir -p "${LAB5_DIR}" "${LAB6_DIR}"
chown -R root:root "${INSTALL_DIR}"
chmod 755 "${INSTALL_DIR}"

# проверим, что бинарники собраны
[[ -x "${LAB5}/build/temp_logger" ]] || { echo "FATAL: нет ${LAB5}/build/temp_logger (собери lab5)"; exit 1; }
[[ -x "${LAB6}/build/temp_gui"    ]] || { echo "FATAL: нет ${LAB6}/build/temp_gui (собери lab6)"; exit 1; }

# копируем бинарники
install -m 755 "${LAB5}/build/temp_logger" "${LAB5_DIR}/temp_logger"
install -m 755 "${LAB6}/build/temp_gui"    "${LAB6_DIR}/temp_gui"

# web (если нужен)
if [[ -d "${LAB5}/web" ]]; then
  rm -rf "${LAB5_DIR}/web"
  cp -a "${LAB5}/web" "${LAB5_DIR}/web"
fi

# db (создадим рядом; будет создаваться/использоваться)
touch "${LAB5_DIR}/temp.db"
chown -R "${KIOSK_USER}:${KIOSK_USER}" "${LAB5_DIR}" "${LAB6_DIR}"

echo "[4/9] systemd сервис для сервера (lab5): oc-temp-logger.service"
cat > /etc/systemd/system/oc-temp-logger.service <<EOF
[Unit]
Description=OC Lab5 Temp Logger/Server (kiosk)
After=network.target

[Service]
Type=simple
User=${KIOSK_USER}
WorkingDirectory=${LAB5_DIR}
ExecStart=${LAB5_DIR}/temp_logger --db ${LAB5_DIR}/temp.db --serve --bind ${BIND} --port ${PORT} --simulate --web-dir ${LAB5_DIR}/web
Restart=always
RestartSec=1
Environment=QT_LOGGING_RULES=*.debug=false

[Install]
WantedBy=multi-user.target
EOF

# если у тебя остался старый oc-temp-server.service — гасим, чтобы не занимал порт
systemctl disable --now oc-temp-server.service >/dev/null 2>&1 || true
systemctl disable --now oc-temp-server.service >/dev/null 2>&1 || true

systemctl daemon-reload
systemctl enable --now oc-temp-logger.service

echo "[5/9] LightDM автологин + openbox сессия"
LIGHTDM_CONF="/etc/lightdm/lightdm.conf"
cp -a "${LIGHTDM_CONF}" "${LIGHTDM_CONF}.bak.$(date +%s)" 2>/dev/null || true

# Включаем автологин
if ! grep -q "^\[Seat:\*\]" "${LIGHTDM_CONF}" 2>/dev/null; then
  printf "\n[Seat:*]\n" >> "${LIGHTDM_CONF}"
fi

# чисто и предсказуемо: дописываем/заменяем нужные ключи
perl -0777 -i -pe '
  s/^\s*autologin-user\s*=.*$/autologin-user=kiosk/m or s/(\[Seat:\*\]\s*\n)/$1autologin-user=kiosk\n/m;
  s/^\s*autologin-user-timeout\s*=.*$/autologin-user-timeout=0/m or s/(\[Seat:\*\]\s*\n)/$1autologin-user-timeout=0\n/m;
' "${LIGHTDM_CONF}"

# openbox Xsession
if [[ ! -f /usr/share/xsessions/openbox.desktop ]]; then
  cat > /usr/share/xsessions/openbox.desktop <<'EOF'
[Desktop Entry]
Name=Openbox
Comment=Openbox session
Exec=openbox-session
TryExec=openbox-session
Type=Application
EOF
fi

# заставим LightDM выбирать openbox
perl -0777 -i -pe '
  s/^\s*user-session\s*=.*$/user-session=openbox/m or s/(\[Seat:\*\]\s*\n)/$1user-session=openbox\n/m;
' "${LIGHTDM_CONF}"

echo "[6/9] Конфиг openbox для киоска (автозапуск GUI + блок “выходов”)"
KHOME="$(eval echo ~${KIOSK_USER})"
install -d -m 700 -o "${KIOSK_USER}" -g "${KIOSK_USER}" "${KHOME}/.config/openbox"

# autostart openbox
cat > "${KHOME}/.config/openbox/autostart" <<EOF
#!/bin/sh
# экраны не гасим
xset s off
xset -dpms
xset s noblank

# курсор спрятать (опционально)
unclutter -idle 0.1 -root &

# убить всё лишнее (если вдруг)
pkill -f xfce4-panel 2>/dev/null || true
pkill -f xfdesktop 2>/dev/null || true

# блок горячих клавиш через xbindkeys (см. конфиг ниже)
xbindkeys &

# ждать сервер
for i in 1 2 3 4 5 6 7 8 9 10; do
  curl -sS "http://${BIND}:${PORT}/api/current" >/dev/null && break
  sleep 0.3
done

# запуск GUI (lab6)
exec ${LAB6_DIR}/temp_gui --base-url http://${BIND}:${PORT}
EOF
chmod +x "${KHOME}/.config/openbox/autostart"
chown "${KIOSK_USER}:${KIOSK_USER}" "${KHOME}/.config/openbox/autostart"

# openbox rules: без рамок + full screen
cat > "${KHOME}/.config/openbox/rc.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <!-- вырубаем дефолтные combos -->
  </keyboard>

  <mouse>
    <!-- отключаем контекстное меню -->
    <context name="Root">
      <mousebind button="Right" action="Press">
        <action name="None"/>
      </mousebind>
    </context>
  </mouse>

  <applications>
    <application name="temp_gui">
      <decor>no</decor>
      <fullscreen>yes</fullscreen>
      <layer>above</layer>
      <focus>yes</focus>
    </application>
  </applications>
</openbox_config>
EOF
chown "${KIOSK_USER}:${KIOSK_USER}" "${KHOME}/.config/openbox/rc.xml"

# xbindkeys: глушим самые популярные “выходы”
cat > "${KHOME}/.xbindkeysrc" <<'EOF'
# Alt+F4
"true"
  Alt + F4

# Alt+Tab
"true"
  Alt + Tab

# Ctrl+Alt+T (терминал)
"true"
  Control + Alt + t

# Super (Win) - часто открывает меню
"true"
  Mod4

# Ctrl+Esc
"true"
  Control + Escape

# Alt+F2
"true"
  Alt + F2
EOF
chown "${KIOSK_USER}:${KIOSK_USER}" "${KHOME}/.xbindkeysrc"

echo "[7/9] Закрываем TTY и Ctrl+Alt+Del (самые жирные “дыры”)"
# отключить ctrl-alt-del
systemctl mask ctrl-alt-del.target >/dev/null 2>&1 || true

# отключить переключение на tty2..tty6 (tty1 оставим на всякий пожарный)
for n in 2 3 4 5 6; do
  systemctl mask "getty@tty${n}.service" >/dev/null 2>&1 || true
done

echo "[8/9] Папка для сдачи в репе (скрипт + README)"
mkdir -p "${REPO}/labs/lab7"
cat > "${REPO}/labs/lab7/README.md" <<EOF
# Lab7 (Kali) Kiosk mode

- User: ${KIOSK_USER}
- Display manager: LightDM autologin into Openbox
- Server: systemd service \`oc-temp-logger.service\` (Lab5) on ${BIND}:${PORT}
- GUI: Openbox autostart launches Lab6 \`temp_gui\` fullscreen

## Install (run as root)
\`\`\`bash
sudo bash labs/lab7_setup_kiosk_kali.sh
\`\`\`

## Verify
- On boot: GUI starts automatically and blocks regular desktop usage.
- API:
  - \`curl http://${BIND}:${PORT}/api/current\`
  - \`curl http://${BIND}:${PORT}/api/stats?from=...&to=...\`

## Rollback (manual)
- Restore lightdm.conf backup: /etc/lightdm/lightdm.conf.bak.*
- Disable services:
  - \`sudo systemctl disable --now oc-temp-logger.service\`
- Unmask:
  - \`sudo systemctl unmask ctrl-alt-del.target\`
  - \`sudo systemctl unmask getty@tty2..tty6\`
EOF

echo "[9/9] Готово. Перезагрузи VM."
echo "Команда: sudo reboot"
