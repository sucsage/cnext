Provided buffers	kernel เลือก recv buffer เอง — ไม่มี buffer management	สูง
Linked SQEs (IOSQE_IO_LINK)	chain recv→process→send เป็น 1 round trip	กลาง
Zero-copy send (send_zc)	ไม่ copy data เข้า kernel — เหมาะกับ response ใหญ่	สูง
HTTP/2	multiplexing + HPACK header compression	สูงมาก