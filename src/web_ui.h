#pragma once

#include <string_view>

namespace sys_info::server {

/**
 * @brief HTML-дашборд
 *
 * Страница статическая: данные забирает JS раз в REFRESH_MS из /api/all.
 * Каждая карточка строится в safe(), поэтому ошибка в одной не ломает остальные.
 */
inline constexpr std::string_view kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>System Info</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: 'Segoe UI', system-ui, sans-serif; background: #0d1117; color: #c9d1d9; padding: 20px; }
h1 { font-size: 1.5em; margin-bottom: 10px; color: #58a6ff; }
h2 { font-size: 1.1em; margin: 15px 0 8px; color: #58a6ff; border-bottom: 1px solid #21262d; padding-bottom: 4px; }
table { border-collapse: collapse; width: 100%; margin-bottom: 10px; background: #161b22; border-radius: 6px; overflow: hidden; }
th, td { padding: 8px 12px; text-align: left; border-bottom: 1px solid #21262d; }
tr:hover { background: #1c2128; }
.status { font-size: 0.85em; color: #8b949e; margin-bottom: 15px; }
.status.error { color: #f85149; }
.bar { height: 8px; background: #21262d; border-radius: 4px; overflow: hidden; margin-top: 2px; }
.bar-fill { height: 100%; border-radius: 4px; }
.fill-green { background: #3fb950; }
.fill-yellow { background: #d29922; }
.fill-red { background: #f85149; }
.fill-blue { background: #58a6ff; }
.grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px; }
@media (max-width: 1400px) { .grid { grid-template-columns: repeat(2, 1fr); } }
@media (max-width: 800px) { .grid { grid-template-columns: 1fr; } }
.card { background: #161b22; border: 1px solid #21262d; border-radius: 8px; padding: 12px; }
.card.error { border-color: #f85149; }
.val { font-size: 1.2em; font-weight: bold; }
.refresh { position: fixed; top: 10px; right: 15px; background: #238636; color: white; border: none; padding: 8px 16px; border-radius: 6px; cursor: pointer; font-size: 0.9em; }
.refresh:hover { background: #2ea043; }
.temp-critical { color: #f85149; font-weight: bold; }
.temp-warn { color: #d29922; }
.temp-ok { color: #3fb950; }
</style>
</head>
<body>
<button class="refresh" onclick="refresh()">Обновить</button>
<h1>System Info</h1>
<div class="status" id="status">Загрузка...</div>
<noscript><div class="status error">Для дашборда нужен JavaScript</div></noscript>
<div class="grid" id="content"></div>
<script>
'use strict';
const REFRESH_MS = 2000;

function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
}
function num(v, d) { return (typeof v === 'number' && isFinite(v)) ? v.toFixed(d) : '—'; }
function pct(v) { return Math.max(0, Math.min(100, Number(v) || 0)); }
function bar(p, cls) {
  if (!cls) cls = p > 80 ? 'fill-red' : p > 60 ? 'fill-yellow' : 'fill-green';
  return '<div class="bar"><div class="bar-fill ' + cls + '" style="width:' + pct(p).toFixed(0) + '%"></div></div>';
}
function tc(t) { return t > 85 ? 'temp-critical' : t > 70 ? 'temp-warn' : 'temp-ok'; }
function fb(b) {
  if (typeof b !== 'number') return '—';
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b / 1024).toFixed(1) + ' KB';
  if (b < 1073741824) return (b / 1048576).toFixed(1) + ' MB';
  return (b / 1073741824).toFixed(1) + ' GB';
}
function deg(t) { return '<span class="' + tc(t) + '">' + num(t, 1) + '&#8451;</span>'; }
function row(k, v) { return '<tr><td>' + k + '</td><td>' + v + '</td></tr>'; }
function section(t) { return '<tr><td colspan="2"><strong>' + t + '</strong></td></tr>'; }
function card(id, title, rows) {
  return rows ? '<div class="card" id="' + id + '"><h2>' + title + '</h2><table>' + rows + '</table></div>' : '';
}

function cpuCard(cpu) {
  const mhz = (cpu.current_mhz && cpu.current_mhz.length) ? cpu.current_mhz[0] : NaN;
  return card('cpu', 'Процессор',
    row('Модель', esc(cpu.model)) +
    row('Ядра (лог / физ)', cpu.logical_cores + ' / ' + cpu.physical_cores) +
    row('Частота', num(mhz, 1) + ' / ' + num(cpu.min_mhz, 0) + '-' + num(cpu.max_mhz, 0) + ' МГц') +
    row('Режим', esc(cpu.governor)));
}

function loadCard(load) {
  let h = row('Общая', '<span class="val ' + tc(load.active_percent) + '">' + num(load.active_percent, 1) + '%</span>') +
    row('Пользователь', '<div class="val">' + num(load.user_percent, 1) + '%</div>' + bar(load.user_percent)) +
    row('Система', '<div class="val">' + num(load.system_percent, 1) + '%</div>' + bar(load.system_percent)) +
    row('IO ожидание', '<div class="val">' + num(load.iowait_percent, 1) + '%</div>' + bar(load.iowait_percent)) +
    row('Load', '<span class="val">' + num(load.load1, 2) + ' / ' + num(load.load5, 2) + ' / ' + num(load.load15, 2) + '</span>');
  for (const c of load.per_core || [])
    h += row('CPU' + c.id, '<span class="val ' + tc(c.percent) + '">' + num(c.percent, 1) + '%</span>');
  return card('load', 'Нагрузка', h);
}

function tempCard(temp) {
  let h = '';
  if (temp.package && temp.package.length) {
    h += section('Процессор');
    for (const s of temp.package) h += row(esc(s.id), deg(s.celsius));
  }
  if (temp.cores && temp.cores.length) {
    const v = temp.cores.map(s => s.celsius);
    const avg = v.reduce((a, b) => a + b, 0) / v.length;
    h += section('Ядра') +
      row('Средняя', deg(avg)) +
      row('Мин / Макс', deg(Math.min(...v)) + ' / ' + deg(Math.max(...v))) +
      '<tr><td colspan="2"><em>По ядрам:</em></td></tr>';
    for (const s of temp.cores) h += '<tr><td style="padding-left:20px">' + esc(s.id) + '</td><td>' + deg(s.celsius) + '</td></tr>';
  }
  if (temp.sensors && temp.sensors.length) {
    h += section('Датчики');
    for (const s of temp.sensors) h += row(esc(s.id), deg(s.celsius));
  }
  return card('temp', 'Температура', h);
}

function memCard(mem) {
  return card('mem', 'Память',
    row('Всего', '<span class="val">' + fb(mem.total) + '</span>') +
    row('Использовано', '<span class="val ' + tc(mem.used_percent) + '">' + fb(mem.used) + '</span>' + bar(mem.used_percent)) +
    row('Доступно', fb(mem.available)) +
    row('Кэш', fb(mem.cached)) +
    row('Буферы', fb(mem.buffers)) +
    row('Swap', fb(mem.swap_used) + ' / ' + fb(mem.swap_total) + ' (' + num(mem.swap_percent, 1) + '%)'));
}

function diskCard(disk) {
  let h = '';
  for (const [mp, m] of Object.entries(disk.mounts || {})) {
    const used = m.total_bytes > 0 ? m.used_bytes / m.total_bytes * 100 : 0;
    h += section(esc(mp) + ' (' + esc(m.device) + ')') +
      row('Файловая система', esc(m.fstype)) +
      row('Занято / Свободно', '<span class="val">' + fb(m.used_bytes) + ' / ' + fb(m.total_bytes - m.used_bytes) + '</span>') +
      row('Использование', bar(used)) +
      row('IO', 'Чтение: ' + fb(m.read_bytes) + ' / Запись: ' + fb(m.write_bytes));
  }
  return card('disk', 'Диски', h);
}

function fanCard(fans) {
  let h = '';
  for (const f of fans.fans || []) h += row(esc(f.id), '<span class="val">' + f.rpm + ' об/мин</span>');
  return card('fans', 'Вентиляторы', h);
}

function hwCard(hw) {
  let h = row('Материнская плата', esc(hw.board_vendor) + ' ' + esc(hw.board_name)) +
    row('BIOS', esc(hw.bios_vendor) + ' v' + esc(hw.bios_version) + ' (' + esc(hw.bios_date) + ')');
  if (hw.product_name) h += row('Продукт', esc(hw.product_name));
  if (hw.battery && hw.battery.status)
    h += row('Батарея', '<span class="val">' + hw.battery.capacity + '%</span> (' + esc(hw.battery.status) + ')');
  for (const g of hw.gpu || []) h += row('Видеокарта', esc(g));
  return card('hw', 'Оборудование', h);
}

function netCard(hw) {
  let h = '';
  for (const n of Object.values(hw.network || {})) {
    const max = n.speed_mbps > 0;
    h += '<tr><td colspan="2"><strong>' + esc(n.name) + '</strong> MAC: ' + esc(n.mac) + '</td></tr>' +
      row('Максимум', max ? n.speed_mbps + ' Мбит/с' : '—') +
      row('Приём', num(n.rate_rx_mbps, 1) + ' Мбит/с' + (max ? bar(n.rate_rx_mbps / n.speed_mbps * 100, 'fill-blue') : '')) +
      row('Передача', num(n.rate_tx_mbps, 1) + ' Мбит/с' + (max ? bar(n.rate_tx_mbps / n.speed_mbps * 100, 'fill-blue') : ''));
  }
  return card('net', 'Сеть', h);
}

function safe(fn, data, title) {
  if (!data) return '';
  try {
    return fn(data);
  } catch (e) {
    console.error(title, e);
    return '<div class="card error"><h2>' + title + '</h2>Ошибка отображения: ' + esc(e.message) + '</div>';
  }
}

let timer = null;
async function refresh() {
  clearTimeout(timer);
  const status = document.getElementById('status');
  try {
    const r = await fetch('/api/all', {cache: 'no-store'});
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const d = await r.json();
    document.getElementById('content').innerHTML =
      safe(cpuCard, d.cpu, 'Процессор') +
      safe(loadCard, d.load, 'Нагрузка') +
      safe(tempCard, d.temp, 'Температура') +
      safe(memCard, d.mem, 'Память') +
      safe(diskCard, d.disk, 'Диски') +
      safe(fanCard, d.fans, 'Вентиляторы') +
      safe(hwCard, d.hw, 'Оборудование') +
      safe(netCard, d.hw, 'Сеть');
    status.textContent = 'Обновлено: ' + new Date().toLocaleString('ru-RU');
    status.classList.remove('error');
  } catch (e) {
    console.error(e);
    status.textContent = 'Нет данных от сервера: ' + e.message;
    status.classList.add('error');
  } finally {
    timer = setTimeout(refresh, REFRESH_MS);
  }
}
refresh();
</script>
</body>
</html>
)HTML";

} // namespace sys_info::server
