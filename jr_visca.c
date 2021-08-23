/*
    Copyright 2021 Jacob Rau
    
    This file is part of libjr_visca.

    libjr_visca is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libjr_visca is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libjr_visca.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "jr_visca.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/**
 * Extract a frame from the given buffer.
 * 
 * `data` is a buffer containing VISCA data. It can be truncated or contain
 * multiple frames.
 * `dataLength` is the count of bytes in `data`.
 * 
 * If at least one full frame is present, it will be written to `frame`.
 * 
 * If less than one full frame is present in `buffer`, returns `0`.
 * 
 * If data corruption is detected (e.g. too many bytes occur before the end-of-frame marker),
 * returns `-1`.
 */
int jr_viscaDataToFrame(uint8_t *data, int dataLength, jr_viscaFrame *frame) {
    // We only decode a frame if the entire frame is present, i.e. 0xff terminator is present.
    int terminatorIndex;
    for (terminatorIndex = 0; terminatorIndex < dataLength; terminatorIndex++) {
        if (data[terminatorIndex] == 0xff) {
            break;
        }
    }

    // If we didn't find a terminator, the index will == dataLength.
    if (!(terminatorIndex < dataLength)) {
        // No bytes consumed, since we're waiting for more bytes to arrive.
        return 0;
    }

    if (terminatorIndex > JR_VISCA_MAX_FRAME_DATA_LENGTH - 1) {
        // All our internal buffers are fixed-length. If the frame exceeds that length, bail.
        return -1;
    }

    if (terminatorIndex == 0) {
        // If no header present, flag an error.
        return -1;
    }

    // First byte is header containing sender and receiver addresses.
    frame->sender = (data[0] >> 4) & 0x7;
    frame->receiver = data[0] & 0x7;

    // N bytes of packet data between header byte and 0xff terminator.
    memcpy(frame->data, data + 1, terminatorIndex - 1);

    frame->dataLength = terminatorIndex - 1;

    return terminatorIndex + 1;
}

typedef struct {
    uint8_t signature[JR_VISCA_MAX_FRAME_DATA_LENGTH];
    uint8_t signatureMask[JR_VISCA_MAX_FRAME_DATA_LENGTH];
    int signatureLength;
    int commandType;
    void (*handleParameters)(jr_viscaFrame* frame, union jr_viscaMessageParameters *messageParameters, bool isDecodingFrame);
} jr_viscaMessageDefinition;

/**
 * `buffer` looks like 0x01 0x02 0x03 0x04
 * Returned result will look like 0x1234
 * This is a common way for VISCA to bit pack things.
 */
int16_t _jr_viscaRead16FromBuffer(uint8_t *buffer) {
    int16_t result = 0;
    result += (buffer[0] & 0xf) * 0x1000;
    result += (buffer[1] & 0xf) * 0x100;
    result += (buffer[2] & 0xf) * 0x10;
    result += (buffer[3] & 0xf);
    return result;
}

/**
 * Given `value` looks like 0x1234
 * `buffer` will look like 0x01 0x02 0x03 0x04
 * We won't touch the upper nibble of each byte-- it may be significant per the specific comand.
 */
void _jr_viscaWrite16ToBuffer(int16_t value, uint8_t *buffer) {
    buffer[0] |= (value >> 12) & 0xf;
    buffer[1] |= (value >> 8) & 0xf;
    buffer[2] |= (value >> 4) & 0xf;
    buffer[3] |= value & 0xf;
}

void jr_visca_handlePanTiltPositionInqResponseParameters(jr_viscaFrame* frame, union jr_viscaMessageParameters *messageParameters, bool isDecodingFrame) {
    if (isDecodingFrame) {
        messageParameters->panTiltPositionInqResponseParameters.panPosition = _jr_viscaRead16FromBuffer(frame->data + 1);
        messageParameters->panTiltPositionInqResponseParameters.tiltPosition = _jr_viscaRead16FromBuffer(frame->data + 5);
    } else {
        _jr_viscaWrite16ToBuffer(messageParameters->panTiltPositionInqResponseParameters.panPosition, frame->data + 1);
        _jr_viscaWrite16ToBuffer(messageParameters->panTiltPositionInqResponseParameters.tiltPosition, frame->data + 5);
    }
}

jr_viscaMessageDefinition definitions[] = {
    {
        {0x09, 0x06, 0x12}, //signature
        {0xff, 0xff, 0xff}, //signatureMask
        3, //signatureLength
        JR_VISCA_MESSAGE_PAN_TILT_POSITION_INQ, //commandType
        NULL //handleParameters
    },
    {
        // pan (signed) = 0xstuv
        // tilt (signed) = 0xwxyz
        //        s     t     u     v     w     y     x     z
        {0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0xff, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0},
        9,
        JR_VISCA_MESSAGE_PAN_TILT_POSITION_INQ_RESPONSE,
        &jr_visca_handlePanTiltPositionInqResponseParameters
    },
    {
        {0x09, 0x04, 0x47},
        {0xff, 0xff, 0xff},
        3,
        JR_VISCA_MESSAGE_ZOOM_POSITION_INQ,
        NULL
    },
    { {}, {}, 0, 0, NULL} // Final definition must have `signatureLength` == 0.
};

void _jr_viscahex_print(char *buf, int buf_size) {
    for (int i = 0; i < buf_size; i++) {
        printf("%02hhx ", buf[i]);
    }
}

void _jr_viscaMemAnd(uint8_t *a, uint8_t *b, uint8_t *output, int length) {
    for (int i = 0; i < length; i++) {
        output[i] = a[i] & b[i];
    }
}

int jr_viscaDecodeFrame(jr_viscaFrame frame, union jr_viscaMessageParameters *messageParameters) {
    int i = 0;
    while (definitions[i].signatureLength) {
        uint8_t maskedFrame[JR_VISCA_MAX_FRAME_DATA_LENGTH];
        _jr_viscaMemAnd(frame.data, definitions[i].signatureMask, maskedFrame, frame.dataLength);
        if (memcmp(maskedFrame, definitions[i].signature, definitions[i].signatureLength) == 0) {
            if (definitions[i].handleParameters != NULL) {
                definitions[i].handleParameters(&frame, messageParameters, true);
            }
            return definitions[i].commandType;
        }
        // printf("definition %d: sig: ", i);
        // _jr_viscahex_print(definitions[i].signature, definitions[i].signatureLength);
        // printf(" sigmask: ");
        // _jr_viscahex_print(definitions[i].signatureMask, definitions[i].signatureLength);
        // printf("\n");
        i++;
    }

    return -1;
}

int jr_viscaEncodeFrame(int messageType, union jr_viscaMessageParameters messageParameters, jr_viscaFrame *frame) {
    int i = 0;
    while (definitions[i].signatureLength) {
        if (messageType == definitions[i].commandType) {
            memcpy(frame->data, definitions[i].signature, definitions[i].signatureLength);
            frame->dataLength = definitions[i].signatureLength;
            if (definitions[i].handleParameters != NULL) {
                definitions[i].handleParameters(frame, &messageParameters, false);
            }
            return 0;
        }
        i++;
    }

    return -1;
}