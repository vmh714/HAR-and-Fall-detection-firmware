# Component: drv_a7680c

## Tổng Quan
Tầng giao tiếp vật lý (Hardware Driver) chuyên trách điều khiển module 4G LTE A7680C qua giao thức UART. Hoạt động độc lập hoàn toàn, không chứa business logic hay FSM.

## Public API & Hoạt Động
- Chịu trách nhiệm gửi/nhận AT command để cấu hình mạng di động.
- Cung cấp API để kích hoạt chuỗi khởi động cứng (Hard Reset) thông qua chân GPIO 18 khi module bị treo.
- Cấu hình Point-to-Point Protocol (PPPoS) để cấp IP trực tiếp cho LwIP stack của ESP-IDF.

*Lưu ý: API chi tiết đang trong quá trình phát triển (Phase 5).*
