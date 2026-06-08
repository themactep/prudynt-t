## Prometheus/Grafana metrics export

This adds a Prometheus text exposition endpoint over prudynt's IPC, with a simple CGI wrapper for HTTP.

What’s included
- per-stream FPS and Bps (bytes/sec): prudynt_stream_fps, prudynt_stream_Bps
- Day/Night live signals: prudynt_daynight_brightness_percent, prudynt_daynight_ev, prudynt_daynight_gr, prudynt_daynight_gb
- ISP denoise controls: prudynt_image_sinter_strength, prudynt_image_temper_strength
- Day/Night state one-hot: prudynt_daynight_state{state="day|night|unknown"}
- RTSP active client sessions: prudynt_rtsp_clients
- Device uptime (seconds): prudynt_uptime_seconds
- Build info (labels): prudynt_build_info{commit,platform} 1

Server-side (already implemented here)
- IPC server gains a "METRICS" command that returns text/plain Prometheus format
- prudyntctl gains a "metrics" subcommand that prints these metrics to stdout

Grafana dashboard
- Import docs/grafana/prudynt-dashboard.json in Grafana
- Panels: RTSP clients, uptime, stream FPS/BW, Day/Night EV & brightness, state timeline

HTTP exposure via BusyBox httpd (example)
Create a CGI wrapper that returns metrics with the correct content type.

1) Place this script as `/www/cgi-bin/metrics`:

```
#!/bin/sh
# BusyBox httpd CGI wrapper for Prometheus metrics
# Path to prudyntctl may vary depending on your firmware packaging
PRUDYNTCTL=${PRUDYNTCTL:-/usr/sbin/prudyntctl}

printf "Content-Type: text/plain\r\n\r\n"
exec "$PRUDYNTCTL" metrics
```

Make sure it is executable:
```
chmod +x /www/cgi-bin/metrics
```

2) Ensure httpd is configured to allow CGI under `/cgi-bin/`.

Prometheus scrape config example
- Scrape the camera’s HTTP endpoint that serves the CGI above:
```
scrape_configs:
  - job_name: prudynt-cam
    metrics_path: /cgi-bin/metrics
    static_configs:
      - targets: ["192.168.1.42"]  # camera IP
```

Notes
- Units: prudynt_stream_Bps is bytes/second; convert to bits/s in Grafana if desired by multiplying by 8.
- Day/Night EV is platform-specific units from IMP ISP.
- If you’re not running BusyBox httpd, adapt the wrapper for your web server.
- The IPC socket is /run/prudynt/prudynt.sock; prudyntctl handles the connection.

Prometheus configuration notes
- If your HTTP server requires basic auth, configure it in Prometheus scrape_config via basic_auth.
- Example:
````
scrape_configs:
  - job_name: prudynt-cam
    metrics_path: /cgi-bin/metrics
    static_configs:
      - targets: ["192.168.1.42:80"]
    basic_auth:
      username: root
      password: "987987"
```
- If you’re using HTTPS with self-signed certs, set `tls_config: insecure_skip_verify: true`.
