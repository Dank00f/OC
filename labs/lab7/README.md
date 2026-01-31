# Lab7 (Kali) Kiosk mode

## What it does
- Autologin via LightDM into user `kiosk`
- Starts Lab5 server via systemd (`oc-temp-logger.service`) on `127.0.0.1:8080`
- Starts Lab6 GUI automatically in X session for user `kiosk`
- Disables tty2..tty6 and Ctrl+Alt+Del target

## Install
```bash
sudo bash labs/lab7_setup_kiosk_min.sh
sudo reboot
```

## Verify
```bash
curl http://127.0.0.1:8080/api/current
```

## Rollback (manual)
- Restore `/etc/lightdm/lightdm.conf.bak.*`
- Disable service:
```bash
sudo systemctl disable --now oc-temp-logger.service
```
- Unmask:
```bash
sudo systemctl unmask ctrl-alt-del.target
sudo systemctl unmask getty@tty2.service getty@tty3.service getty@tty4.service getty@tty5.service getty@tty6.service
```
