#include "server.h"
#include "collector.h"
#include <httplib.h>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>

namespace sys_info::server {

static std::atomic<bool> g_running{false};
static std::unique_ptr<httplib::Server> g_server;

static std::string build_html() {
    static const char* css =
        "* { margin: 0; padding: 0; box-sizing: border-box; }\n"
        "body { font-family: 'Segoe UI', system-ui, sans-serif; background: #0d1117; color: #c9d1d9; padding: 20px; }\n"
        "h1 { font-size: 1.5em; margin-bottom: 10px; color: #58a6ff; }\n"
        "h2 { font-size: 1.1em; margin: 15px 0 8px; color: #58a6ff; border-bottom: 1px solid #21262d; padding-bottom: 4px; }\n"
        "table { border-collapse: collapse; width: 100%; margin-bottom: 10px; background: #161b22; border-radius: 6px; overflow: hidden; }\n"
        "th, td { padding: 8px 12px; text-align: left; border-bottom: 1px solid #21262d; }\n"
        "th { background: #1c2128; color: #58a6ff; font-weight: 600; }\n"
        "tr:hover { background: #1c2128; }\n"
        ".status { font-size: 0.85em; color: #8b949e; margin-bottom: 15px; }\n"
        ".bar { height: 8px; background: #21262d; border-radius: 4px; overflow: hidden; margin-top: 2px; }\n"
        ".bar-fill { height: 100%; border-radius: 4px; }\n"
        ".fill-green { background: #3fb950; }\n"
        ".fill-yellow { background: #d29922; }\n"
        ".fill-red { background: #f85149; }\n"
        ".fill-blue { background: #58a6ff; }\n"
    ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 12px; }\n"
         ".card { background: #161b22; border: 1px solid #21262d; border-radius: 8px; padding: 12px; display: inline-block; width: auto; }\n"
        ".val { font-size: 1.2em; font-weight: bold; }\n"
        ".unit { color: #8b949e; font-size: 0.85em; }\n"
        ".refresh { position: fixed; top: 10px; right: 15px; background: #238636; color: white; border: none; padding: 8px 16px; border-radius: 6px; cursor: pointer; font-size: 0.9em; }\n"
        ".refresh:hover { background: #2ea043; }\n"
        ".temp-critical { color: #f85149; font-weight: bold; }\n"
        ".temp-warn { color: #d29922; }\n"
        ".temp-ok { color: #3fb950; }";

    std::ostringstream html;
    html << "<!DOCTYPE html>\n"
         "<html lang=\"ru\">\n"
         "<head><meta charset=\"UTF-8\">\n"
         "<title>System Info</title>\n"
         "<style>" << css << "</style>\n"
         "</head><body>\n"
         "<button class=\"refresh\" onclick=\"location.reload()\">Refresh</button>\n"
         "<h1>System Info</h1>\n"
         "<div class=\"status\" id=\"timestamp\">Loading...</div>\n"
         "<div class=\"grid\" id=\"content\"></div>\n"
         "<script>\n"
         "async function f(p){try{let r=await fetch(p);return await r.json();}catch(e){return null;}}\n"
         "function bar(p){let c='fill-green';if(p>80)c='fill-red';else if(p>60)c='fill-yellow';return '<div class=\"bar\"><div class=\"bar-fill '+c+'\" style=\"width:'+p.toFixed(0)+'%\"></div></div>';}\n"
         "function tc(t){if(t>85)return'temp-critical';if(t>70)return'temp-warn';return'temp-ok';}\n"
         "function fb(b){if(b<1024)return b+' B';if(b<1048576)return(b/1024).toFixed(1)+' KB';if(b<1073741824)return(b/1048576).toFixed(1)+' MB';return(b/1073741824).toFixed(1)+' GB';}\n"
        "async function render(){\n"
         "  let cpu=await f('/api/cpu'),load=await f('/api/load'),temp=await f('/api/temp'),mem=await f('/api/mem'),disk=await f('/api/disk'),fans=await f('/api/fans'),hw=await f('/api/hw');\n"
         "  let h='';\n"
         "  if(cpu){h+='<div class=\"card\" id=\"cpu\"><h2>Процессор</h2><table>';\n"
         "    h+='<tr><td>Модель</td><td>'+cpu.model+'</td></tr>';\n"
         "    h+='<tr><td>Ядра (лог / физ)</td><td>'+cpu.logical_cores+' / '+cpu.physical_cores+'</td></tr>';\n"
         "    h+='<tr><td>Частота</td><td>'+cpu.current_mhz[0].toFixed(1)+' / '+cpu.min_mhz.toFixed(0)+'-'+cpu.max_mhz.toFixed(0)+' МГц</td></tr>';\n"
         "    h+='<tr><td>Режим</td><td>'+cpu.governor+'</td></tr>';\n"
         "    h+='</table></div>';}\n"
         "  if(load){h+='<div class=\"card\" id=\"load\"><h2>Нагрузка</h2><table>';\n"
         "    h+='<tr><td>Общая</td><td><span class=\"val '+tc(load.active_percent)+'\">'+load.active_percent.toFixed(1)+'%</span></td></tr>';\n"
         "    h+='<tr><td>Пользователь</td><td><div class=\"val\">'+load.user_percent.toFixed(1)+'%</div>'+bar(load.user_percent)+'</td></tr>';\n"
         "    h+='<tr><td>Система</td><td><div class=\"val\">'+load.system_percent.toFixed(1)+'%</div>'+bar(load.system_percent)+'</td></tr>';\n"
         "    h+='<tr><td>IO ожидание</td><td><div class=\"val\">'+load.iowait_percent.toFixed(1)+'%</div>'+bar(load.iowait_percent)+'</td></tr>';\n"
         "    h+='<tr><td>Load</td><td><span class=\"val\">'+load.load1.toFixed(2)+' / '+load.load5.toFixed(2)+' / '+load.load15.toFixed(2)+'</span></td></tr>';\n"
         "    if(load.per_core&&load.per_core.length){for(let c of load.per_core){let pct=c.total>0?((c.active/c.total)*100):0;\n"
         "      h+='<tr><td>CPU'+c.id+'</td><td><span class=\"val '+tc(pct)+'\">'+pct.toFixed(1)+'%</span></td></tr>';}}\n"
         "    h+='</table></div>';}\n"
         "  if(temp){h+='<div class=\"card\" id=\"temp\"><h2>Температура</h2><table>';\n"
         "    if(temp.package&&temp.package.length){for(let s of temp.package){h+='<tr><td colspan=\"2\"><strong>Процессор</strong></td></tr>';\n"
         "      h+='<tr><td>'+s.id+'</td><td class=\"'+tc(s.celsius)+'\">'+s.celsius.toFixed(1)+'&#8451;</td></tr>';}}\n"
         "    if(temp.cores&&temp.cores.length){let avg=0,mn=100,mx=0;\n"
         "      for(let s of temp.cores){avg+=s.celsius;if(s.celsius<mn)mn=s.celsius;if(s.celsius>mx)mx=s.celsius;}\n"
         "      avg/=temp.cores.length;\n"
         "      h+='<tr><td colspan=\"2\"><strong>Ядра</strong></td></tr>';\n"
         "      h+='<tr><td>Средняя</td><td class=\"'+tc(avg)+'\">'+avg.toFixed(1)+'&#8451;</td></tr>';\n"
         "      h+='<tr><td>Мин / Макс</td><td colspan=\"2\"><span class=\"'+tc(mn)+'\">'+mn.toFixed(1)+'&#8451;</span> / <span class=\"'+tc(mx)+'\">'+mx.toFixed(1)+'&#8451;</span></td></tr>';\n"
         "      h+='<tr><td colspan=\"2\"><em>По ядрам:</em></td></tr>';\n"
         "      for(let s of temp.cores){h+='<tr><td style=\"padding-left:20px\">'+s.id+'</td><td class=\"'+tc(s.celsius)+'\">'+s.celsius.toFixed(1)+'&#8451;</td></tr>';}}\n"
         "    if(temp.sensors&&temp.sensors.length){for(let s of temp.sensors){h+='<tr><td>'+s.id+'</td><td class=\"'+tc(s.celsius)+'\">'+s.celsius.toFixed(1)+'&#8451;</td></tr>';}}\n"
         "    h+='</table></div>';}\n"
         "  if(mem){h+='<div class=\"card\" id=\"mem\"><h2>Память</h2><table>';\n"
         "    h+='<tr><td>Всего</td><td class=\"val\">'+fb(mem.total)+'</td></tr>';\n"
         "    h+='<tr><td>Использовано</td><td><span class=\"val '+tc(mem.used_percent)+'\">'+fb(mem.used)+'</span>'+bar(mem.used_percent)+'</td></tr>';\n"
         "    h+='<tr><td>Доступно</td><td class=\"'+tc(mem.used_percent)+'\">'+fb(mem.available)+'</td></tr>';\n"
         "    h+='<tr><td>Кэш</td><td>'+fb(mem.cached)+'</td></tr>';\n"
         "    h+='<tr><td>Буферы</td><td>'+fb(mem.buffers)+'</td></tr>';\n"
         "    h+='<tr><td>Swap</td><td>'+fb(mem.swap_used)+' / '+fb(mem.swap_total)+' ('+mem.swap_percent.toFixed(1)+'%)</td></tr>';\n"
         "    h+='</table></div>';}\n"
         "  if(disk){h+='<div class=\"card\" id=\"disk\"><h2>Диски</h2><table>';\n"
         "    for(let mp in disk.mounts){let m=disk.mounts[mp];\n"
         "      let usedPct=m.total_bytes>0?(m.used_bytes/m.total_bytes*100):0;\n"
         "      h+='<tr><td colspan=\"2\"><strong>'+mp+'</strong> ('+m.device+')</td></tr>';\n"
         "      h+='<tr><td>Файловая система</td><td>'+m.fstype+'</td></tr>';\n"
         "      h+='<tr><td>Занято / Свободно</td><td><span class=\"val\">'+fb(m.used_bytes)+' / '+fb(m.total_bytes-m.used_bytes)+'</span></td></tr>';\n"
         "      h+='<tr><td>Использование</td><td>'+bar(usedPct)+'</td></tr>';\n"
         "      h+='<tr><td>IO</td><td>Чтение: '+fb(m.read_bytes)+' / Запись: '+fb(m.write_bytes)+'</td></tr>';}\n"
         "    h+='</table></div>';}\n"
         "  if(fans&&fans.fans&&fans.fans.length){h+='<div class=\"card\" id=\"fans\"><h2>Вентиляторы</h2><table>';\n"
         "    for(let fl of fans.fans){h+='<tr><td>'+fl.id+'</td><td class=\"val\">'+fl.rpm+' об/мин</td></tr>';}\n"
         "    h+='</table></div>';}\n"
         "  if(hw){h+='<div class=\"card\" id=\"hw\"><h2>Оборудование</h2><table>';\n"
         "    h+='<tr><td>Материнская плата</td><td>'+hw.board_vendor+' '+hw.board_name+'</td></tr>';\n"
         "    h+='<tr><td>BIOS</td><td>'+hw.bios_vendor+' v'+hw.bios_version+' ('+hw.bios_date+')</td></tr>';\n"
         "    if(hw.product_name)h+='<tr><td>Продукт</td><td>'+hw.product_name+'</td></tr>';\n"
         "    if(hw.battery){h+='<tr><td>Батарея</td><td><span class=\"val\">'+hw.battery.capacity+'%</span> ('+hw.battery.status+')</td></tr>';}\n"
         "    if(hw.gpu&&hw.gpu.length){for(let g of hw.gpu)h+='<tr><td>Видеокарта</td><td>'+g+'</td></tr>';}\n"
         "    if(hw.network){for(let n in hw.network){let ni=hw.network[n];\n"
         "      h+='<tr><td>Сеть</td><td><strong>'+ni.name+'</strong> MAC: '+ni.mac+(ni.speed_mbps>0?' ('+ni.speed_mbps+' Мбит/с)':'')+'</td></tr>';}}\n"
         "    h+='</table></div>';}\n"
         "  let el=document.getElementById('content');\n"
         "  if(el.innerHTML!==h)el.innerHTML=h;\n"
         "  let ts=document.querySelector('.status');\n"
         "  if(ts)ts.textContent='Обновлено: '+new Date().toLocaleString('ru-RU');\n"
         "}\n"
         "render();setInterval(render,2000);\n"
         "</script></body></html>";

    return html.str();
}

static std::string handle_api(const std::string& path) {
    if (path == "/api/cpu") {
        auto cpu = collector::read_cpu_info();
        nlohmann::json j;
        nlohmann::to_json(j, cpu);
        return j.dump(2);
    }
    if (path == "/api/load") {
        auto load = collector::read_cpu_load();
        nlohmann::json j;
        nlohmann::to_json(j, load);
        return j.dump(2);
    }
    if (path == "/api/temp") {
        auto temp = collector::read_temperatures();
        nlohmann::json j;
        nlohmann::to_json(j, temp);
        return j.dump(2);
    }
    if (path == "/api/fans") {
        auto fans = collector::read_fans();
        nlohmann::json j;
        nlohmann::to_json(j, fans);
        return j.dump(2);
    }
    if (path == "/api/mem") {
        auto mem = collector::read_memory();
        nlohmann::json j;
        nlohmann::to_json(j, mem);
        return j.dump(2);
    }
    if (path == "/api/disk") {
        auto disk = collector::read_disk_info();
        nlohmann::json j;
        nlohmann::to_json(j, disk);
        return j.dump(2);
    }
    if (path == "/api/hw") {
        auto hw = collector::read_hardware_info();
        nlohmann::json j;
        nlohmann::to_json(j, hw);
        return j.dump(2);
    }
    if (path == "/api/health" || path == "/api/all") {
        auto status = collector::read_all();
        nlohmann::json j;
        nlohmann::to_json(j, status.cpu);
        j["load"] = status.load;
        j["temp"] = status.temp;
        j["fans"] = status.fans;
        j["mem"] = status.mem;
        j["disk"] = status.disk;
        j["hw"] = status.hw;
        return j.dump(2);
    }
    return "{\"error\":\"not found\"}";
}

bool start_server(int port, const std::vector<Route>& extra_routes) {
    g_server = std::make_unique<httplib::Server>();

    g_server->Get("/api/cpu", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_api("/api/cpu"), "application/json");
    });
    g_server->Get("/api/load", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_api("/api/load"), "application/json");
    });
    g_server->Get("/api/temp", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_api("/api/temp"), "application/json");
    });
    g_server->Get("/api/fans", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_api("/api/fans"), "application/json");
    });
    g_server->Get("/api/mem", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_api("/api/mem"), "application/json");
    });
    g_server->Get("/api/disk", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_api("/api/disk"), "application/json");
    });
    g_server->Get("/api/hw", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_api("/api/hw"), "application/json");
    });
    g_server->Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_api("/api/health"), "application/json");
    });
    g_server->Get("/api/all", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_api("/api/all"), "application/json");
    });
    g_server->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(build_html(), "text/html; charset=utf-8");
    });

    for (auto& r : extra_routes) {
        g_server->Get(r.path, [h = r.handler](const httplib::Request&, httplib::Response& res) {
            res.set_content(h(), "application/json");
        });
    }

    g_running = true;
    bool ok = g_server->listen("0.0.0.0", port);
    if (!ok) {
        g_running = false;
    }
    return ok;
}

void stop_server() {
    if (g_server) {
        g_server->stop();
        g_running = false;
    }
}

} // namespace sys_info::server
