#!/bin/bash
# Сценарий внутри контейнера с systemd (запускает tests/deb_install.sh): установка пакета из /deb,
# служба, обновление с сохранением настроек, выключенная служба, remove, purge, повторная установка.
set -u
fail=0
ok() { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fail=1; }
wait_http() { for _ in $(seq 50); do curl -sf "http://127.0.0.1:$1/api/health" >/dev/null && return 0; sleep 0.2; done; return 1; }
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq /deb/*.deb >/dev/null 2>&1 || bad "apt install"
systemctl is-active -q hostpulse && ok "служба запущена" || { bad "служба не запущена"; journalctl -u hostpulse --no-pager | tail -5; }
systemctl is-enabled -q hostpulse && ok "служба включена" || bad "служба не включена"
wait_http 8080 && ok "отвечает на 8080" || bad "нет ответа на 8080"
ss -ltn | grep -q '0.0.0.0:8080' && ok "слушает 0.0.0.0:8080" || bad "не 0.0.0.0: $(ss -ltn | grep 8080)"
pid=$(systemctl show -p MainPID --value hostpulse)
user=$(stat -c %U "/proc/$pid" 2>/dev/null)
[ "$user" = hostpulse ] && ok "работает от динамического пользователя hostpulse" || bad "пользователь службы: '$user'"
curl -sf http://127.0.0.1:8080/ | grep -q '<title>Состояние системы</title>' && ok "веб-страница отдаётся" || bad "нет страницы"
curl -sf http://127.0.0.1:8080/api/all | grep -q '"logical_cores"' && ok "API отдаёт данные" || bad "нет данных в /api/all"
journalctl -u hostpulse --no-pager | grep -q 'запуск сервера на 0.0.0.0:8080' && ok "сообщение о запуске в журнале" \
    || bad "нет сообщения о запуске: $(journalctl -u hostpulse --no-pager | tail -3)"
journalctl -u hostpulse --no-pager | grep -q 'Ошибка сбора данных' && bad "ошибки сбора данных под ограничениями службы" \
    || ok "без ошибок сбора данных"

sed -i 's/--port 8080/--port 8081/' /etc/default/hostpulse
apt-get install -y -qq --reinstall /deb/*.deb >/dev/null 2>&1 || bad "переустановка"
grep -q 8081 /etc/default/hostpulse && ok "настройки сохранены при обновлении" || bad "настройки затёрты"
wait_http 8081 && ok "после обновления перезапущена на 8081" || bad "нет ответа на 8081"

systemctl disable -q --now hostpulse
apt-get install -y -qq --reinstall /deb/*.deb >/dev/null 2>&1
systemctl is-enabled -q hostpulse && bad "выключенная служба включилась при обновлении" || ok "выключенная администратором остаётся выключенной"
systemctl enable -q --now hostpulse

apt-get remove -y -qq hostpulse >/dev/null 2>&1 || bad "remove"
systemctl is-active -q hostpulse && bad "служба работает после удаления" || ok "remove: служба остановлена"
[ -e /etc/default/hostpulse ] && ok "remove: настройки остались" || bad "remove: настройки удалены"
apt-get purge -y -qq hostpulse >/dev/null 2>&1 || bad "purge"
[ ! -e /etc/default/hostpulse ] && ok "purge: настройки удалены" || bad "purge: настройки остались"

apt-get install -y -qq /deb/*.deb >/dev/null 2>&1 || bad "повторная установка"
wait_http 8080 && ok "повторная установка: служба на 8080" || bad "повторная установка"
exit $fail
