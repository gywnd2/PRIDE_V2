Import("env")

from pathlib import Path


def parse_credentials(path):
    ssid1 = ""
    password1 = ""
    ssid2 = ""
    password2 = ""

    if not path.exists():
        print("[wifi-cred] wifi_credentials.txt not found, using empty credentials")
        return ssid1, password1, ssid2, password2, 0

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip().upper()
        value = value.strip()
        if key == "SSID":
            ssid1 = value
        elif key == "PASSWORD":
            password1 = value
        elif key == "SSID1":
            ssid1 = value
        elif key == "PASSWORD1":
            password1 = value
        elif key == "SSID2":
            ssid2 = value
        elif key == "PASSWORD2":
            password2 = value

    cred1_valid = bool(ssid1 and password1)
    cred2_valid = bool(ssid2 and password2)
    count = (1 if cred1_valid else 0) + (1 if cred2_valid else 0)

    if count == 0:
        print("[wifi-cred] no valid SSID/PASSWORD pair found")
    else:
        print(f"[wifi-cred] loaded {count} credential set(s)")
    return ssid1, password1, ssid2, password2, count


def c_escape(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


project_dir = Path(env.subst("$PROJECT_DIR"))
cred_path = project_dir / "wifi_credentials.txt"
out_header = project_dir / "include" / "WifiCredentialsLocal.h"

ssid1, password1, ssid2, password2, count = parse_credentials(cred_path)

content = """#ifndef WIFI_CREDENTIALS_LOCAL_H
#define WIFI_CREDENTIALS_LOCAL_H

#define WIFI_CRED_SSID1 "{ssid1}"
#define WIFI_CRED_PASSWORD1 "{password1}"
#define WIFI_CRED_SSID2 "{ssid2}"
#define WIFI_CRED_PASSWORD2 "{password2}"
#define WIFI_CRED_COUNT {count}
#define WIFI_CRED_LOADED {loaded}

/* Backward-compatible aliases (credential set #1) */
#define WIFI_CRED_SSID WIFI_CRED_SSID1
#define WIFI_CRED_PASSWORD WIFI_CRED_PASSWORD1

#endif
""".format(
    ssid1=c_escape(ssid1),
    password1=c_escape(password1),
    ssid2=c_escape(ssid2),
    password2=c_escape(password2),
    count=count,
    loaded=1 if count > 0 else 0,
)

out_header.parent.mkdir(parents=True, exist_ok=True)
out_header.write_text(content, encoding="utf-8")
