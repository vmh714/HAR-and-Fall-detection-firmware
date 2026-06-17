# Tập lệnh AT — A7680C (SIMCom A76XX): LTE-only + PPPoS

> Tổng hợp từ AT Command Manual dòng SIMCom A76XX (gồm A7680C), phục vụ cấu hình
> khóa 4G LTE và thiết lập internet gateway qua PPPoS. Mục cuối map từng lệnh vào code firmware để đối chiếu.

## Tóm tắt nhanh

| Lệnh | Mục đích | Lưu NVRAM | Dùng ở đâu trong firmware |
|---|---|---|---|
| `AT+CNMP=38` | Khóa **LTE-only** (chặn quét 2G/3G) | ✅ AUTO_SAVE | `svc_network.c` (gửi qua esp_modem) |
| `AT+CFUN=0/1` | Tắt/bật RF khi đổi chế độ mạng | — | `svc_network.c` (bao quanh CNMP) |
| `AT+CPIN?` | Kiểm tra SIM sẵn sàng | — | `svc_network.c` (chẩn đoán) |
| `AT+CSQ` | Đo cường độ sóng | — | `svc_network.c` (chẩn đoán) |
| `AT+CGDCONT=1,"IP","v-internet"` | Khai báo APN (PDP context) | ✅ AUTO_SAVE | esp_modem tự gửi (từ `dce_config.apn`) |
| `ATD*99***1#` | Dial vào PPP → `CONNECT` | — | esp_modem tự gửi khi `set_mode(DATA)` |
| PWRKEY pulse | Bật/tắt nguồn module | — | `drv_a7680c.c` (GPIO) |

---

## 1. AT Commands for Network

### `AT+CNMP` — Chọn chế độ mạng ưu tiên
Bảng giá trị `<mode>`: `2` = Automatic · `13` = GSM only · `14` = WCDMA only · **`38` = LTE only**.
- **Lưu NVRAM:** Có (**AUTO_SAVE**) — có hiệu lực ngay và giữ qua reboot.
- Cú pháp: Test `AT+CNMP=?` → `+CNMP: (2,13,14,38)` · Read `AT+CNMP?` → `+CNMP: 38` · Write `AT+CNMP=38`.

### `AT+CMNB` — Cat-M / NB-IoT
Không có trong manual A76XX. A7680C là **LTE Cat-1**, không phải Cat-M/NB-IoT → **không cần** lệnh này.

### `AT+CEREG` — Trạng thái đăng ký EPS (LTE)
- Read: `AT+CEREG?` → `+CEREG: <n>,<stat>[,<tac>,<ci>]`. URC: `AT+CEREG=<n>` (0=tắt, 1=bật, 2=kèm vị trí).
- `<stat>`: **`1`** = đã đăng ký (home), **`5`** = đã đăng ký (roaming). Chưa kết nối: `0` (không tìm), `2` (đang tìm), `3` (bị từ chối), `4` (ngoài vùng phủ).

### `AT+CPSI?` — Thông tin hệ thống (loại mạng + band)
`+CPSI: <System Mode>,<Operation Mode>,<MCC>-<MNC>,<TAC>,<SCellID>,<PCellID>,<Band>,<earfcn>,...`
- `<System Mode>` = **`LTE`** khi đang 4G; `<Band>` ví dụ `EUTRAN-BAND3`.
- Ví dụ: `+CPSI: LTE,Online,460-01,0x230A,175499523,318,EUTRAN-BAND3,1650,5,0,21,67,255,19`.

### `AT+COPS` / `AT+CSQ` — Chọn nhà mạng / Đo sóng
- `AT+COPS=0` → tự động quét & chọn nhà mạng.
- `AT+CSQ` → `+CSQ: <rssi>,<ber>`. Thang `<rssi>`: `0` ≤ -113dBm · `2..30` ≈ -109..-53dBm (lớn = khỏe) · `31` ≥ -51dBm · `99` = không xác định.

### `AT+CNBP` — Khóa băng tần (tùy chọn)
`AT+CNBP=<mode>[,<lte_mode>]`, AUTO_SAVE. `<lte_mode>` là hex 64-bit theo bit shift `1 << <lte_pos>` (pos 0=BAND1, 2=BAND3, 6=BAND7, 27=BAND28). "Any" = `0x000007FF3FDF3FFF`. Dùng nếu muốn ép cứng vào band nhà mạng.

---

## 2. AT Commands for Packet Domain

### `AT+CGDCONT` — Định nghĩa PDP Context (APN)
`AT+CGDCONT=<cid>[,<PDP_type>[,<APN>...]]`, AUTO_SAVE.
- `<cid>`: 1–15 · `<PDP_type>`: `"IP"` (IPv4), `"IPV6"`, `"IPV4V6"` · `<APN>`: tên điểm truy cập.
- Ví dụ: `AT+CGDCONT=1,"IP","v-internet"`. Read `AT+CGDCONT?` · Test `AT+CGDCONT=?`.

### `AT+CGATT` — Attach/Detach Packet Domain
`AT+CGATT=<state>` (`0`=detach, `1`=attach). Read `AT+CGATT?` → `+CGATT: 1`. **NO_SAVE**.

### Dial vào PPP — `ATD*99***1#` (khuyến nghị)
- `ATD*99***<cid>#`: dạng tường minh, `<cid>` cuối ép dùng đúng PDP context đã khai (`AT+CGDCONT=1,...`). Dùng `ATD*99***1#` an toàn hơn `ATD*99#` (dạng rút gọn, tự map vào cid mặc định).
- Sau dial, module trả **`CONNECT`** → bắt đầu thương lượng PPP (LCP/IPCP) qua UART.
- Thay thế: `AT+CGDATA="",1` (hoặc `AT+CGDATA="PPP",1`) cũng đưa vào data state và trả `CONNECT`.

---

## 3. AT Commands for Status / SIM / Serial

### `AT+CFUN` — Mức hoạt động
`<fun>`: `1` = full (bật RF+SIM) · `0` = minimum (tắt RF & SIM, UART vẫn chạy) · `4` = airplane (tắt RF) · `5` = factory test · `6` = reset · `7` = offline.
- CNMP có hiệu lực ngay nên **không bắt buộc** cycle CFUN. Nhưng best-practice: `CFUN=0` → `CNMP=38` → `CFUN=1` để baseband scan/attach lại sạch trong LTE-only.

### `AT+CPIN?` — Trạng thái SIM
`READY` = SIM sẵn sàng (cần check OK khi init) · `SIM PIN`/`SIM PUK` = chờ PIN/PUK · `PH-SIM PIN`/`PH-NET PIN` = khóa máy/khóa mạng.

### `AT+IPR` / `AT+IFC` — Serial
- `AT+IPR`: baud mặc định **115200** (8N1). Không có auto-baud (không nhận giá trị 0). `AT+IPR` chỉ tạm thời → lưu vĩnh viễn bằng `AT+IPREX` (AUTO_SAVE).
- `AT+IFC`: flow control mặc định `0,0` (None). Bật HW flow control: `AT+IFC=2,2` (cần nối RTS/CTS).

---

## 4. Hardware Timing — chân PWRKEY

| Thao tác | Yêu cầu |
|---|---|
| **Power-ON** | Kéo PWRKEY xuống Low **~50 ms (Ton typical)** rồi nhả |
| **Power-OFF** | Kéo PWRKEY xuống Low **≥ 2.5 s (Toff min)** |
| **Toff-on (chờ giữa tắt→bật)** | **≥ 2 s** trước khi bật lại, tránh boot lỗi |

> A7680C có Ton **nhanh hơn** dòng module cũ (50ms vs ~1s). Kéo/thả PWRKEY quá ngắn → module bật bất thường.

---

## 5. Đối chiếu với code firmware (verification)

| Bước | Lệnh AT thực tế | Ai gửi | File |
|---|---|---|---|
| Bật nguồn | xung PWRKEY low ~100ms | `drv_a7680c_power_on()` | `drv_a7680c.c` |
| Sync | `AT` (retry 5×) | `svc_network` | `svc_network.c` |
| Check SIM | `AT+CPIN?` | `svc_network` | `svc_network.c` |
| Khóa LTE | `AT+CFUN=0` → `AT+CNMP=38` → `AT+CFUN=1` | `svc_network` | `svc_network.c` |
| Đo sóng | `AT+CSQ` | `svc_network` | `svc_network.c` |
| Khai APN | `AT+CGDCONT=1,"IP","v-internet"` | **esp_modem** (từ `dce_config.apn`) | tự động |
| Dial PPP | `ATD*99***1#` → `CONNECT` | **esp_modem** (`set_mode(DATA)`) | tự động |
| PPP/IP | thương lượng LCP/IPCP | **esp_modem + LwIP** | tự động |

**Kết luận verify:** code khớp instruction. Lệnh trực tiếp ta gửi (`CPIN?`, `CFUN`, `CNMP=38`, `CSQ`) đúng cú pháp/giá trị; phần `CGDCONT` + dial `ATD*99***1#` do esp_modem đảm nhận đúng chuẩn. Timing PWRKEY đã chỉnh về Ton ~100ms / Toff 2.5s theo datasheet.
