//---------------------------------------------------------------------------
#include "TinyFrame.h"
#include <string.h>
//---------------------------------------------------------------------------

// Compatibility with ESP8266 SDK
#ifdef ICACHE_FLASH_ATTR
	#define _TF_FN ICACHE_FLASH_ATTR
#else
	#define _TF_FN
#endif

enum TFState {
	TFState_SOF = 0,      //!< Wait for SOF
	TFState_LEN,          //!< Wait for Number Of Bytes
	TFState_HEAD_CKSUM,   //!< Wait for header Checksum
	TFState_ID,           //!< Wait for ID
	TFState_TYPE,         //!< Wait for message type
	TFState_DATA,         //!< Receive payload
	TFState_DATA_CKSUM    //!< Wait for Checksum
};

typedef struct _IdListener_struct {
	TF_ID id;
	TF_LISTENER fn;
} IdListener;

typedef struct _TypeListener_struct_ {
	TF_TYPE type;
	TF_LISTENER fn;
} TypeListener;

typedef struct _GenericListener_struct_ {
	TF_LISTENER fn;
} GenericListener;

/**
 * Frame parser internal state
 */
static struct TinyFrameStruct {
	/* Own state */
	TF_PEER peer_bit;       //!< Own peer bit (unqiue to avoid msg ID clash)
	TF_ID next_id;          //!< Next frame / frame chain ID

	/* Parser state */
	enum TFState state;
	int parser_timeout_ticks;
	TF_ID id;               //!< Incoming packet ID
	TF_LEN len;             //!< Payload length
	uint8_t data[TF_MAX_PAYLOAD]; //!< Data byte buffer
	size_t rxi;             //!< Byte counter
	TF_CKSUM cksum;         //!< Checksum calculated of the data stream
	TF_CKSUM ref_cksum;     //!< Reference checksum read from the message
	TF_TYPE type;           //!< Collected message type number
	bool discard_data;      //!< Set if (len > TF_MAX_PAYLOAD) to read the frame, but ignore the data.

	/* --- Callbacks --- */

	/* Transaction callbacks */
	IdListener id_listeners[TF_MAX_ID_LST];
	TypeListener type_listeners[TF_MAX_TYPE_LST];
	GenericListener generic_listeners[TF_MAX_GEN_LST];

	size_t count_id_lst;
	size_t count_type_lst;
	size_t count_generic_lst;

	// Buffer for building frames
	uint8_t sendbuf[TF_MAX_PAYLOAD + TF_OVERHEAD_BYTES];
} tf;

//region Checksums

#if TF_CKSUM_TYPE == 0

// NONE
#define CKSUM_RESET(cksum)
#define CKSUM_ADD(cksum, byte)
#define CKSUM_FINALIZE(cksum)

#elif TF_CKSUM_TYPE == 8

// ~XOR
#define CKSUM_RESET(cksum) do { cksum = 0; } while (0)
#define CKSUM_ADD(cksum, byte) do { cksum ^= byte; } while(0)
#define CKSUM_FINALIZE(cksum)  do { cksum = (TF_CKSUM)~cksum; } while(0)

#elif TF_CKSUM_TYPE == 16

/** CRC table for the CRC-16. The poly is 0x8005 (x^16 + x^15 + x^2 + 1) */
static const uint16_t crc16_table[256] = {
	0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
	0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
	0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
	0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
	0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
	0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
	0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
	0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
	0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
	0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
	0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
	0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
	0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
	0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
	0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
	0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
	0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
	0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
	0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
	0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
	0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
	0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
	0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
	0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
	0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
	0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
	0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
	0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
	0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
	0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
	0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
	0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};

static inline uint16_t crc16_byte(uint16_t cksum, const uint8_t byte)
{
	return (cksum >> 8) ^ crc16_table[(cksum ^ byte) & 0xff];
}

#define CKSUM_RESET(cksum) do { cksum = 0; } while (0)
#define CKSUM_ADD(cksum, byte) do { cksum = crc16_byte(cksum, byte); } while(0)
#define CKSUM_FINALIZE(cksum)

#elif TF_CKSUM_TYPE == 32

static const uint32_t crc32_table[] = { /* CRC polynomial 0xedb88320 */
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

static inline uint32_t crc32_byte(uint32_t cksum, const uint8_t byte)
{
	return (crc32_table[((cksum) ^ ((uint8_t)byte)) & 0xff] ^ ((cksum) >> 8));
}

#define CKSUM_RESET(cksum) do { cksum = (TF_CKSUM)0xFFFFFFFF; } while (0)
#define CKSUM_ADD(cksum, byte) do { cksum = crc32_byte(cksum, byte); } while(0)
#define CKSUM_FINALIZE(cksum)  do { cksum = (TF_CKSUM)~cksum; } while(0)

#endif

//endregion


void _TF_FN TF_Init(TF_PEER peer_bit)
{
	// Zero it out
	memset(&tf, 0, sizeof(struct TinyFrameStruct));

	tf.peer_bit = peer_bit;
}

//region Listeners

bool _TF_FN TF_AddIdListener(TF_ID frame_id, TF_LISTENER cb)
{
	size_t i;
	for (i = 0; i < TF_MAX_ID_LST; i++) {
		if (tf.id_listeners[i].fn == NULL) {
			tf.id_listeners[i].fn = cb;
			tf.id_listeners[i].id = frame_id;
			if (i >= tf.count_id_lst) {
				tf.count_id_lst = i + 1;
			}
			return true;
		}
	}
	return false;
}

bool _TF_FN TF_AddTypeListener(TF_TYPE frame_type, TF_LISTENER cb)
{
	size_t i;
	for (i = 0; i < TF_MAX_TYPE_LST; i++) {
		if (tf.type_listeners[i].fn == NULL) {
			tf.type_listeners[i].fn = cb;
			tf.type_listeners[i].type = frame_type;
			if (i >= tf.count_type_lst) {
				tf.count_type_lst = i + 1;
			}
			return true;
		}
	}
	return false;
}

bool _TF_FN TF_AddGenericListener(TF_LISTENER cb)
{
	size_t i;
	for (i = 0; i < TF_MAX_GEN_LST; i++) {
		if (tf.generic_listeners[i].fn == NULL) {
			tf.generic_listeners[i].fn = cb;
			if (i >= tf.count_generic_lst) {
				tf.count_generic_lst = i + 1;
			}
			return true;
		}
	}
	return false;
}

bool _TF_FN TF_RemoveIdListener(TF_ID frame_id)
{
	size_t i;
	for (i = 0; i < tf.count_id_lst; i++) {
		if (tf.id_listeners[i].fn != NULL
			&& tf.id_listeners[i].id == frame_id) {
			tf.id_listeners[i].fn = NULL;
			if (i == tf.count_id_lst - 1) {
				tf.count_id_lst--;
			}
			return true;
		}
	}
	return false;
}

bool _TF_FN TF_RemoveTypeListener(TF_TYPE type)
{
	size_t i;
	for (i = 0; i < tf.count_type_lst; i++) {
		if (tf.type_listeners[i].fn != NULL
			&& tf.type_listeners[i].type == type) {
			tf.type_listeners[i].fn = NULL;
			if (i == tf.count_type_lst - 1) {
				tf.count_type_lst--;
			}
			return true;
		}
	}
	return false;
}

bool _TF_FN TF_RemoveGenericListener(TF_LISTENER cb)
{
	size_t i;
	for (i = 0; i < tf.count_generic_lst; i++) {
		if (tf.generic_listeners[i].fn == cb) {
			tf.generic_listeners[i].fn = NULL;
			if (i == tf.count_generic_lst - 1) {
				tf.count_generic_lst--;
			}
			return true;
		}
	}
	return false;
}

/** Handle a message that was just collected & verified by the parser */
static void _TF_FN TF_HandleReceivedMessage(TF_ID frame_id, TF_TYPE type, uint8_t *data, TF_LEN data_len)
{
	size_t i;

	// Any listener can consume the message (return true),
	// or let someone else handle it.

	// The loop upper bounds are the highest currently used slot index
	// (or close to it, depending on the order of listener removals)

	// ID listeners first
	for (i = 0; i < tf.count_id_lst; i++) {
		if (tf.id_listeners[i].fn && (tf.id_listeners[i].id == frame_id)) {
			if (tf.id_listeners[i].fn(frame_id, type, data, data_len)) {
				return;
			}
		}
	}

	// Type listeners
	for (i = 0; i < tf.count_type_lst; i++) {
		if (tf.type_listeners[i].fn && (tf.type_listeners[i].type == type)) {
			if (tf.type_listeners[i].fn(frame_id, type, data, data_len)) {
				return;
			}
		}
	}

	// Generic listeners
	for (i = 0; i < tf.count_generic_lst; i++) {
		if (tf.generic_listeners[i].fn) {
			if (tf.generic_listeners[i].fn(frame_id, type, data, data_len)) {
				return;
			}
		}
	}
}

//endregion Listeners

void _TF_FN TF_Accept(const uint8_t *buffer, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++) {
		TF_AcceptChar(buffer[i]);
	}
}

void _TF_FN TF_ResetParser(void)
{
	tf.state = TFState_SOF;
}

/** SOF was received */
static void _TF_FN TF_ParsBeginFrame(void) {
	// Reset state vars
	CKSUM_RESET(tf.cksum);
	#if TF_USE_SOF_BYTE
		CKSUM_ADD(tf.cksum, TF_SOF_BYTE);
	#endif

	tf.discard_data = false;

	// Enter ID state
	tf.state = TFState_ID;
	tf.rxi = 0;
}

void _TF_FN TF_AcceptChar(unsigned char c)
{
	// Timeout - clear
	if (tf.parser_timeout_ticks >= TF_PARSER_TIMEOUT_TICKS) {
		TF_ResetParser();
	}
	tf.parser_timeout_ticks = 0;

// DRY snippet - collect multi-byte number from the input stream
#define COLLECT_NUMBER(dest, type) dest = (type)(((dest) << 8) | c); \
								   if (++tf.rxi == sizeof(type))

	#if !TF_USE_SOF_BYTE
		if (tf.state == TFState_SOF) {
			TF_ParsBeginFrame();
		}
	#endif

	switch (tf.state) {
		case TFState_SOF:
			if (c == TF_SOF_BYTE) {
				TF_ParsBeginFrame();
			}
			break;

		case TFState_ID:
			CKSUM_ADD(tf.cksum, c);
			COLLECT_NUMBER(tf.id, TF_ID) {
				// Enter LEN state
				tf.state = TFState_LEN;
				tf.rxi = 0;
			}
			break;

		case TFState_LEN:
			CKSUM_ADD(tf.cksum, c);
			COLLECT_NUMBER(tf.len, TF_LEN) {
				// Enter TYPE state
				tf.state = TFState_TYPE;
				tf.rxi = 0;
			}
			break;

		case TFState_TYPE:
			CKSUM_ADD(tf.cksum, c);
			COLLECT_NUMBER(tf.type, TF_TYPE) {
				#if TF_CKSUM_TYPE == 0
					tf.state = TFState_DATA;
					tf.rxi = 0;
				#else
					// enter HEAD_CKSUM state
					tf.state = TFState_HEAD_CKSUM;
					tf.rxi = 0;
					tf.ref_cksum = 0;
				#endif
			}
			break;

		case TFState_HEAD_CKSUM:
			COLLECT_NUMBER(tf.ref_cksum, TF_CKSUM) {
				// Check the header checksum against the computed value
				CKSUM_FINALIZE(tf.cksum);

				if (tf.cksum != tf.ref_cksum) {
					TF_ResetParser();
					break;
				}

				if (tf.len == 0) {
					TF_HandleReceivedMessage(tf.id, tf.type, NULL, 0);
					TF_ResetParser();
					break;
				}

				// Enter DATA state
				tf.state = TFState_DATA;
				tf.rxi = 0;

				CKSUM_RESET(tf.cksum); // Start collecting the payload

				if (tf.len >= TF_MAX_PAYLOAD) {
					// ERROR - frame too long. Consume, but do not store.
					tf.discard_data = true;
				}
			}
			break;

		case TFState_DATA:
			if (tf.discard_data) {
				tf.rxi++;
			} else {
				CKSUM_ADD(tf.cksum, c);
				tf.data[tf.rxi++] = c;
			}

			if (tf.rxi == tf.len) {
				#if TF_CKSUM_TYPE == 0
					// All done
					TF_HandleReceivedMessage(tf.id, tf.type, tf.data, tf.len);
					TF_ResetParser();
				#else
					// Enter DATA_CKSUM state
					tf.state = TFState_DATA_CKSUM;
					tf.rxi = 0;
					tf.ref_cksum = 0;
				#endif
			}
			break;

		case TFState_DATA_CKSUM:
			COLLECT_NUMBER(tf.ref_cksum, TF_CKSUM) {
				// Check the header checksum against the computed value
				CKSUM_FINALIZE(tf.cksum);
				if (!tf.discard_data && tf.cksum == tf.ref_cksum) {
					TF_HandleReceivedMessage(tf.id, tf.type, tf.data, tf.len);
				}

				TF_ResetParser();
			}
			break;
	}
}

/**
 * Compose a frame (used internally by TF_Send and TF_Respond).
 * The frame can be sent using TF_WriteImpl(), or received by TF_Accept()
 *
 * @param outbuff - buffer to store the result in
 * @param msgid - message ID is stored here, if not NULL
 * @param type - message type
 * @param data - data buffer
 * @param len - payload size in bytes
 * @param explicit_id - ID to use in the frame (8-bit)
 * @param use_expl_id - whether to use the previous param
 * @return nr of bytes in outbuff used by the frame, TF_ERROR (-1) on failure
 */
static int _TF_FN TF_Compose(uint8_t *outbuff, TF_ID *id_ptr,
			      TF_TYPE type,
				  const uint8_t *data, TF_LEN data_len,
				  TF_ID explicit_id, bool use_expl_id)
{
	int i;
	uint8_t b;
	TF_ID id;
	TF_CKSUM cksum;
	int pos = 0;

	CKSUM_RESET(cksum);

	// sanitize len
	if (data_len > TF_MAX_PAYLOAD) {
		return TF_ERROR;
	}

	// Gen ID
	if (use_expl_id) {
		id = explicit_id;
	}
	else {
		id = (TF_ID) (tf.next_id++ & TF_ID_MASK);
		if (tf.peer_bit) {
			id |= TF_ID_PEERBIT;
		}
	}

	if (id_ptr != NULL)
		*id_ptr = id;

	// DRY helper for writing a multi-byte variable to the buffer
#define WRITENUM_BASE(type, num, xtra) \
	for (i = sizeof(type)-1; i>=0; i--) { \
		b = (uint8_t)(num >> (i*8) & 0xFF); \
		outbuff[pos++] = b; \
		xtra; \
	}
	
#define _NOOP()
#define WRITENUM(type, num)       WRITENUM_BASE(type, num, _NOOP())
#define WRITENUM_CKSUM(type, num) WRITENUM_BASE(type, num, CKSUM_ADD(cksum, b))

	// --- Start ---
	CKSUM_RESET(cksum);

	#if TF_USE_SOF_BYTE
		outbuff[pos++] = TF_SOF_BYTE;
		CKSUM_ADD(cksum, TF_SOF_BYTE);
	#endif

	WRITENUM_CKSUM(TF_ID, id);
	WRITENUM_CKSUM(TF_LEN, data_len);
	WRITENUM_CKSUM(TF_TYPE, type);

	#if TF_CKSUM_TYPE != 0
		CKSUM_FINALIZE(cksum);
		WRITENUM(TF_CKSUM, cksum);
	#endif

	// --- payload begin ---
	if (data_len > 0) {
		CKSUM_RESET(cksum);

		// DATA
		for (i = 0; i < data_len; i++) {
			b = data[i];
			outbuff[pos++] = b;
			CKSUM_ADD(cksum, b);
		}

		#if TF_CKSUM_TYPE != 0
			CKSUM_FINALIZE(cksum);
			WRITENUM(TF_CKSUM, cksum);
		#endif
	}

	return pos;
}

bool _TF_FN TF_Send(TF_TYPE type,
			 const uint8_t *payload, TF_LEN payload_len,
			 TF_LISTENER listener,
			 TF_ID *id_ptr)
{
	TF_ID msgid = 0;
	int len;
	len = TF_Compose(tf.sendbuf, &msgid, type, payload, payload_len, 0, false);
	if (len == TF_ERROR) return false;

	if (listener) TF_AddIdListener(msgid, listener);
	if (id_ptr) *id_ptr = msgid;

	TF_WriteImpl((const uint8_t *) tf.sendbuf, (TF_LEN)len);
	return true;
}

// Like TF_Send, but with explicit frame ID
bool _TF_FN TF_Respond(TF_TYPE type,
				const uint8_t *data, TF_LEN data_len,
				TF_ID frame_id)
{
	int len;
	len = TF_Compose(tf.sendbuf, NULL, type, data, data_len, frame_id, true);
	if (len == TF_ERROR) return false;

	TF_WriteImpl(tf.sendbuf, (TF_LEN)len);
	return true;
}

/**
 * Like TF_Send(), but with no data
 */
bool _TF_FN TF_Send0(TF_TYPE type,
			  TF_LISTENER listener,
			  TF_ID *id_ptr)
{
	return TF_Send(type, NULL, 0, listener, id_ptr);
}

/**
 * Like TF_Send(), but with just 1 data byte
 */
bool _TF_FN TF_Send1(TF_TYPE type, uint8_t b1,
			  TF_LISTENER listener,
			  TF_ID *id_ptr)
{
	unsigned char b[] = {b1};
	return TF_Send(type, b, 1, listener, id_ptr);
}

/**
 * Like TF_Send(), but with just 2 data bytes
 */
bool _TF_FN TF_Send2(TF_TYPE type, uint8_t b1, uint8_t b2,
			  TF_LISTENER listener,
			  TF_ID *id_ptr)
{
	unsigned char b[] = {b1, b2};
	return TF_Send(type, b, 2, listener, id_ptr);
}

/** Timebase hook - for timeouts */
void _TF_FN TF_Tick(void)
{
	if (tf.parser_timeout_ticks < TF_PARSER_TIMEOUT_TICKS) {
		tf.parser_timeout_ticks++;
	}
}

