#!/bin/sh

# Low-wear power telemetry for A/B tests.
# Samples stay under the firmware's /tmp area; its filesystem type is recorded.
# The log is copied to the SD card only once, at shutdown.

sysdir=${TELMI_SYSDIR:-/mnt/SDCARD/.tmp_update}
savesdir=${TELMI_SAVESDIR:-/mnt/SDCARD/Saves}
ramdir=${TELMI_RAMDIR:-/tmp}
flagfile=$savesdir/.powerTelemetry
intervalfile=$savesdir/.powerTelemetryInterval
devicefile=$savesdir/.powerDevice
ramlog=$ramdir/telmi-power.csv
persistdir=$savesdir/Diagnostics/power
finished=0
sleep_pid=

sanitize_label() {
    tr -cd 'A-Za-z0-9_.-' | cut -c 1-32
}

read_label() {
    path=$1
    fallback=$2
    value=
    if [ -r "$path" ]; then
        value=$(sanitize_label < "$path")
    fi
    if [ -n "$value" ]; then
        printf '%s' "$value"
    else
        printf '%s' "$fallback"
    fi
}

read_first() {
    read_value=NA
    for path in "$@"; do
        if [ -r "$path" ]; then
            value=
            IFS= read -r value < "$path"
            if [ -n "$value" ]; then
                read_value=$value
                return
            fi
        fi
    done
}

find_pid() {
    found_pid=
    for comm_path in /proc/[0-9]*/comm; do
        [ -r "$comm_path" ] || continue
        comm=
        IFS= read -r comm < "$comm_path"
        if [ "$comm" = "$1" ]; then
            found_pid=${comm_path#/proc/}
            found_pid=${found_pid%/comm}
            return
        fi
    done
}

read_process() {
    pid=$1
    proc_ticks=NA
    proc_rss=NA
    proc_voluntary=NA
    proc_involuntary=NA
    if [ -z "$pid" ] || [ ! -r "/proc/$pid/stat" ]; then
        return
    fi

    stat_line=
    IFS= read -r stat_line < "/proc/$pid/stat"
    # Intentional field split: storyTeller and batmon comm values contain no spaces.
    # shellcheck disable=SC2086
    set -- $stat_line
    if [ "$#" -ge 15 ]; then
        shift 13
        proc_ticks=$(($1 + $2))
    fi

    while read -r key value rest; do
        case "$key" in
            VmRSS:)
                proc_rss=$value
                ;;
            voluntary_ctxt_switches:)
                proc_voluntary=$value
                ;;
            nonvoluntary_ctxt_switches:)
                proc_involuntary=$value
                ;;
        esac
    done < "/proc/$pid/status"
}

extract_json_number() {
    json_value=NA
    json_input=$1
    json_key=$2
    case "$json_input" in
        *\"$json_key\":*)
            json_value=${json_input#*\""$json_key"\":}
            json_value=${json_value%%,*}
            json_value=${json_value%%\}*}
            while [ "${json_value# }" != "$json_value" ]; do
                json_value=${json_value# }
            done
            ;;
    esac
}

read_json_file_number() {
    json_value=NA
    json_path=$1
    json_key=$2
    [ -r "$json_path" ] || return
    while IFS= read -r json_line; do
        extract_json_number "$json_line" "$json_key"
        [ "$json_value" = NA ] || return
    done < "$json_path"
}

sample() {
    epoch=$(date +%s)
    read -r uptime _ < /proc/uptime
    read_first "$ramdir/percBat"
    battery=$read_value
    read_first "$ramdir/.axp_result"
    axp=$read_value
    extract_json_number "$axp" voltage
    voltage=$json_value
    extract_json_number "$axp" charging
    charging=$json_value
    read_first "$ramdir/telmi-display-state"
    display=$read_value

    cpu_total=NA
    cpu_idle=NA
    processes=NA
    while read -r key f1 f2 f3 f4 f5 f6 f7 f8 _; do
        case "$key" in
            cpu)
                cpu_idle=$f4
                # guest/guest_nice are already included in user/nice by Linux.
                cpu_total=$((f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8))
                ;;
            processes)
                processes=$f1
                break
                ;;
        esac
    done < /proc/stat
    read -r load1 _ < /proc/loadavg
    read_first \
        /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq \
        /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
    frequency=$read_value
    read_first \
        /sys/devices/system/cpu/cpufreq/policy0/scaling_governor \
        /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
    governor=$read_value
    read_first \
        /sys/class/thermal/thermal_zone0/temp \
        /sys/class/hwmon/hwmon0/temp1_input
    temperature=$read_value
    read_first /sys/devices/soc0/soc/1f003400.pwm/pwm/pwmchip0/pwm0/enable
    pwm=$read_value

    find_pid storyTeller
    story_pid=$found_pid
    read_process "$story_pid"
    story_ticks=$proc_ticks
    story_rss=$proc_rss
    story_voluntary=$proc_voluntary
    story_involuntary=$proc_involuntary

    find_pid batmon
    batmon_pid=$found_pid
    read_process "$batmon_pid"
    batmon_ticks=$proc_ticks
    batmon_rss=$proc_rss
    batmon_voluntary=$proc_voluntary
    batmon_involuntary=$proc_involuntary

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$epoch" "$uptime" "$battery" "$voltage" "$charging" "$display" \
        "$pwm" "$cpu_total" "$cpu_idle" "$processes" "$load1" "$frequency" "$governor" "$temperature" \
        "${story_pid:-NA}" "$story_ticks" "$story_rss" "$story_voluntary" "$story_involuntary" \
        "${batmon_pid:-NA}" "$batmon_ticks" "$batmon_rss" "$batmon_voluntary" "$batmon_involuntary" \
        >> "$ramlog"
}

finish() {
    if [ "$finished" -eq 1 ]; then
        return 0
    fi
    finished=1
    if [ -n "$sleep_pid" ]; then
        kill "$sleep_pid" 2> /dev/null
        sleep_pid=
    fi

    if ! sample; then
        report_flush_failure "cannot append the final RAM sample"
        return 1
    fi
    if ! mkdir -p "$persistdir"; then
        report_flush_failure "cannot create $persistdir"
        return 1
    fi
    stamp=$(date +%Y%m%d-%H%M%S)
    read -r uptime_tag _ < /proc/uptime
    uptime_tag=${uptime_tag%%.*}
    destination=$persistdir/power-$device_label-$variant-$stamp-u$uptime_tag.csv
    temporary_destination=$destination.tmp
    if ! cp "$ramlog" "$temporary_destination" ||
       ! mv -f "$temporary_destination" "$destination"; then
        rm -f "$temporary_destination" 2> /dev/null
        report_flush_failure "cannot copy the RAM log to $destination"
        return 1
    fi
    return 0
}

report_flush_failure() {
    flush_error=$1
    printf 'powerlog: %s; RAM log remains at %s\n' "$flush_error" "$ramlog" >&2
    printf '%s\n' "$flush_error" > "$ramdir/telmi-power-flush-error" 2> /dev/null || true
    # This is written only on failure, so it does not add wear during normal use.
    printf '%s\n' "$flush_error" > "$savesdir/telmi-power-flush-error.txt" 2> /dev/null || true
}

terminate() {
    trap - EXIT
    if finish; then
        exit 0
    fi
    exit 1
}

on_exit() {
    exit_status=$?
    trap - EXIT
    if ! finish; then
        exit_status=1
    fi
    exit "$exit_status"
}

if [ ! -r "$flagfile" ]; then
    exit 0
fi

interval=60
if [ -r "$intervalfile" ]; then
    requested_interval=$(cat "$intervalfile" 2> /dev/null)
    case "$requested_interval" in
        *[!0-9]*|'')
            ;;
        *)
            if [ "$requested_interval" -ge 30 ] && [ "$requested_interval" -le 3600 ]; then
                interval=$requested_interval
            fi
            ;;
    esac
fi

device_label=$(read_label "$devicefile" device)
variant=$(read_label "$flagfile" unknown)
read_first "$ramdir/deviceModel"
device_id=$read_value
read_first "$ramdir/screen_resolution"
resolution=$read_value
read_first "$sysdir/telmiVersion/version.txt"
telmi_version=$read_value
read_json_file_number "$savesdir/.parameters" storyScreenOnDuration
screen_window=$json_value
read_json_file_number "$savesdir/.parameters" audioVolumeStartup
startup_volume=$json_value
read_json_file_number "$savesdir/.parameters" screenBrightnessStartup
startup_brightness=$json_value

ram_filesystem=unknown
root_filesystem=unknown
while read -r _ mount_path mount_type _; do
    case "$mount_path" in
        /)
            root_filesystem=$mount_type
            ;;
        "$ramdir")
            ram_filesystem=$mount_type
            ;;
    esac
done < /proc/mounts
[ "$ram_filesystem" != unknown ] || ram_filesystem="inherited-$root_filesystem"

firmware=$(/etc/fw_printenv miyoo_version 2> /dev/null | tr -d '\r\n')
[ -n "$firmware" ] || firmware=NA

{
    printf '# format=telmi-power-v1\n'
    printf '# device_label=%s\n' "$device_label"
    printf '# variant=%s\n' "$variant"
    printf '# device_id=%s\n' "$device_id"
    printf '# resolution=%s\n' "$resolution"
    printf '# sample_interval_s=%s\n' "$interval"
    printf '# telmi_version=%s\n' "$telmi_version"
    printf '# firmware=%s\n' "$firmware"
    printf '# ram_log_path=%s\n' "$ramlog"
    printf '# ram_filesystem=%s\n' "$ram_filesystem"
    printf '# story_screen_on_s=%s\n' "$screen_window"
    printf '# startup_volume=%s\n' "$startup_volume"
    printf '# startup_brightness=%s\n' "$startup_brightness"
    printf 'epoch,uptime_s,battery_pct,voltage_raw,charging,display_on,pwm_enabled,cpu_total_ticks,cpu_idle_ticks,processes_started,load1,cpu_freq_khz,governor,temperature_raw,story_pid,story_ticks,story_rss_kb,story_voluntary_cs,story_involuntary_cs,batmon_pid,batmon_ticks,batmon_rss_kb,batmon_voluntary_cs,batmon_involuntary_cs\n'
} > "$ramlog"

trap terminate HUP INT TERM
trap on_exit EXIT

while true; do
    sample
    sleep "$interval" &
    sleep_pid=$!
    wait "$sleep_pid"
    sleep_pid=
done
