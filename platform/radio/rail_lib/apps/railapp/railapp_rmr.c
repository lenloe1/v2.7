/***************************************************************************//**
 * @file
 * @brief Source file for RAIL Ram Modem Reconfiguration functionality
 *******************************************************************************
 * # License
 * <b>Copyright 2018 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/

#include <stdlib.h>
#include <stdint.h>

#include "rail.h"
#include "em_core.h"
#include "em_device.h"

#include "railapp_rmr.h"
#include "app_common.h"
#include "command_interpreter.h"
#include "response_print.h"

#ifdef CONFIGURATION_HEADER
#include CONFIGURATION_HEADER
#endif

typedef struct RMR_State{
  uint32_t phyInfo[RMR_PHY_INFO_LEN];
  uint8_t irCalConfig[RMR_IRCAL_LEN];
  uint32_t modemConfigEntry[RMR_MODEM_CONFIG_LEN];
  RAIL_FrameType_t frameTypeConfig;
  uint16_t frameLenList[RMR_FRAME_LENGTH_LIST_LEN];
  uint32_t frameCodingTable[RMR_FRAME_CODING_TABLE_LEN];
  RAIL_ChannelConfigEntryAttr_t generatedEntryAttr;
  RAIL_ChannelConfigEntry_t generatedChannels[1];
  __ALIGNED(4) uint8_t convDecodeBuffer[RMR_CONV_DECODE_BUFFER_LEN];
  RAIL_ChannelConfig_t channelConfig;
} RMR_State_t;

static RMR_State_t *rmrState = NULL;

static volatile bool mallocLock = false;
#define RAIL_HEAP_SIZE 0x1400
__ALIGNED(4) uint8_t railMallocBuffer[RAIL_HEAP_SIZE];

void *railMalloc(uint32_t size)
{
  CORE_DECLARE_IRQ_STATE;
  void *buffer;
  CORE_ENTER_CRITICAL();
  if (mallocLock || size > sizeof(railMallocBuffer)) {
    buffer = NULL;
  } else {
    mallocLock = true;
    buffer = railMallocBuffer;
  }
  CORE_EXIT_CRITICAL();
  return buffer;
}

// Internal commands
RAIL_Status_t Rmr_writeRmrStructure(RAIL_RMR_StructureIndex_t structure, uint16_t offset, uint8_t count, uint8_t *dataPtr);
RAIL_Status_t Rmr_updateConfigurationPointer(uint8_t structToModify, uint16_t offset, uint8_t structToPointTo);
RAIL_Status_t Rmr_reconfigureModem(RAIL_Handle_t railHandle);

//----------------------------------------------------------------------------
// Ram Modem Reconfiguration Commands
//-----------------------------------------------------------------------------
RAIL_Status_t Rmr_updateConfigurationPointer(uint8_t structToModify, uint16_t offset, uint8_t structToPointTo)
{
  uint32_t structPointer = 0u; // NULL
  // First get the addres of the structure we are trying to reference
  switch (structToPointTo) {
    case (RMR_STRUCT_PHY_INFO): {
      structPointer = (uint32_t)&(rmrState->phyInfo);
      break;
    }
    case (RMR_STRUCT_IRCAL_CONFIG): {
      structPointer = (uint32_t)&(rmrState->irCalConfig);
      break;
    }
    case (RMR_STRUCT_FRAME_TYPE_CONFIG): {
      structPointer = (uint32_t)&(rmrState->frameTypeConfig);
      break;
    }
    case (RMR_STRUCT_FRAME_LENGTH_LIST): {
      structPointer = (uint32_t)&(rmrState->frameLenList);
      break;
    }
    case (RMR_STRUCT_FRAME_CODING_TABLE): {
      structPointer = (uint32_t)&(rmrState->frameCodingTable);
      break;
    }
    case (RMR_STRUCT_CONV_DECODE_BUFFER): {
      structPointer = (uint32_t)&(rmrState->convDecodeBuffer);
      break;
    }
    case (RMR_STRUCT_NULL): {
      structPointer = 0u; // NULL
      break;
    }
    default: {
      // Error, unrecognized structure
      return RAIL_STATUS_INVALID_PARAMETER;
    }
  }
  switch (structToModify) {
    case (RMR_STRUCT_MODEM_CONFIG): {
      if (offset >= RMR_MODEM_CONFIG_LEN) {
        return RAIL_STATUS_INVALID_PARAMETER;
      }
      rmrState->modemConfigEntry[offset] = structPointer;
      break;
    }
    case (RMR_STRUCT_PHY_INFO): {
      if (offset >= RMR_PHY_INFO_LEN) {
        return RAIL_STATUS_INVALID_PARAMETER;
      }
      rmrState->phyInfo[offset] = structPointer;
      break;
    }
    case (RMR_STRUCT_FRAME_TYPE_CONFIG): {
      if (offset != 0) {
        return RAIL_STATUS_INVALID_PARAMETER;
      }
      rmrState->frameTypeConfig.frameLen = (uint16_t *)structPointer;
      break;
    }
    default: {
      // Error unrecognized structure
      return RAIL_STATUS_INVALID_PARAMETER;
    }
  }
  return RAIL_STATUS_NO_ERROR;
}

RAIL_Status_t Rmr_reconfigureModem(RAIL_Handle_t railHandle)
{
  RAIL_RadioState_t currentState = RAIL_GetRadioState(railHandle);
  if (currentState != RAIL_RF_STATE_IDLE) {
    return RAIL_STATUS_INVALID_STATE;
  }
  disableIncompatibleProtocols(RAIL_PTI_PROTOCOL_CUSTOM);

  // Always include frame type length functionality when using RMR or config
  // channels won't work for frame type based lengths.
  RAIL_IncludeFrameTypeLength(railHandle);

  // Configure with the downloaded channel configuration.
  RAIL_ConfigChannels(railHandle, &rmrState->channelConfig, &RAILCb_RadioConfigChanged);

  // Make sure that we stay in idle after the reconfiguration.
  RAIL_Idle(railHandle, RAIL_IDLE_FORCE_SHUTDOWN_CLEAR_FLAGS, false);

  return RAIL_STATUS_NO_ERROR;
}

//-----------------------------------------------------------------------------
// RMR CI Commands
//-----------------------------------------------------------------------------
RAIL_Status_t Rmr_writeRmrStructure(RAIL_RMR_StructureIndex_t structure, uint16_t offset, uint8_t count, uint8_t *dataPtr)
{
  uint8_t *targetStruct;
  uint32_t size;
  switch (structure) {
    case (RMR_STRUCT_PHY_INFO): {
      size = sizeof(rmrState->phyInfo);
      targetStruct = (uint8_t *) &(rmrState->phyInfo);
      break;
    }
    case (RMR_STRUCT_IRCAL_CONFIG): {
      size = sizeof(rmrState->irCalConfig);
      targetStruct = (uint8_t *) &(rmrState->irCalConfig);
      break;
    }
    case (RMR_STRUCT_MODEM_CONFIG): {
      size = sizeof(rmrState->modemConfigEntry);
      targetStruct = (uint8_t *)&(rmrState->modemConfigEntry);
      break;
    }
    case (RMR_STRUCT_FRAME_TYPE_CONFIG): {
      size = sizeof(rmrState->frameTypeConfig);
      targetStruct = (uint8_t *) &(rmrState->frameTypeConfig);
      break;
    }
    case (RMR_STRUCT_FRAME_LENGTH_LIST): {
      size = sizeof(rmrState->frameLenList);
      targetStruct = (uint8_t *) &(rmrState->frameLenList);
      break;
    }
    case (RMR_STRUCT_FRAME_CODING_TABLE): {
      size = RMR_FRAME_CODING_TABLE_LEN * sizeof(uint32_t);
      targetStruct = (uint8_t *) &(rmrState->frameCodingTable);
      break;
    }
    case (RMR_STRUCT_CHANNEL_CONFIG_ATTRIBUTES): {
      size = sizeof(rmrState->generatedEntryAttr);
      targetStruct = (uint8_t *) &(rmrState->generatedEntryAttr);
      break;
    }
    case (RMR_STRUCT_CHANNEL_CONFIG_ENTRY): {
      size = sizeof(rmrState->generatedChannels);
      targetStruct = (uint8_t *) &(rmrState->generatedChannels);
      break;
    }
    default: {
      return RAIL_STATUS_INVALID_PARAMETER;
      break;
    }
  }
  // Check that we are not writing out of bounds
  if ((offset + count) > size) {
    return RAIL_STATUS_INVALID_PARAMETER;
  }
  targetStruct += offset;
  while (count-- != 0u) {
    *targetStruct = *dataPtr;
    targetStruct++;
    dataPtr++;
  }
  return RAIL_STATUS_NO_ERROR;
}

static bool rmrInit(char **argv)
{
  if (rmrState == NULL) {
    rmrState = railMalloc(sizeof(RMR_State_t));
    if (rmrState == NULL) {
      responsePrintError(argv[RMR_CI_RESPONSE], 0x86, "Error allocating RMR memory.");
      return false;
    }
    rmrState->channelConfig.phyConfigBase = &(rmrState->modemConfigEntry[0]);
    rmrState->channelConfig.phyConfigDeltaSubtract = NULL;
    rmrState->channelConfig.configs = rmrState->generatedChannels;
    rmrState->channelConfig.length = 1;
    rmrState->channelConfig.signature = 0U;
    rmrState->generatedChannels[0].phyConfigDeltaAdd = NULL;
    rmrState->generatedChannels[0].attr = &rmrState->generatedEntryAttr;
  }
  return true;
}

void CI_writeRmrStructure(int argc, char **argv)
{
  uint8_t count = ciGetUnsigned(argv[RMR_CI_COUNT]);
  uint8_t bufferedData[RMR_ARGUMENT_BUFFER_SIZE];
  if (!rmrInit(argv)) {
    return;
  }
  if (argc != (count + RMR_CI_DATA_START)) {
    responsePrintError(argv[RMR_CI_RESPONSE], 0x80, "Argument count does not match number of arguments.");
    return;
  }
  if (count > RMR_ARGUMENT_BUFFER_SIZE) {
    responsePrintError(argv[RMR_CI_RESPONSE], 0x81, "Number of arguments greater than local buffer.");
    return;
  }
  RAIL_RMR_StructureIndex_t structure = ciGetUnsigned(argv[RMR_CI_RMR_STRUCTURE]);
  uint16_t offset = ciGetUnsigned(argv[RMR_CI_OFFSET]);
  uint8_t i;
  for (i = 0u; i < count; i++) {
    bufferedData[i] = ciGetUnsigned(argv[RMR_CI_DATA_START + i]);
  }
  if (Rmr_writeRmrStructure(structure, offset, count, bufferedData) != RAIL_STATUS_NO_ERROR) {
    responsePrintError(argv[RMR_CI_RESPONSE], 0x82, "Error writing to structure.");
    return;
  }
  responsePrint(argv[RMR_CI_RESPONSE], "CommandStatus:Success");
  return;
}

void CI_updateConfigurationPointer(int argc, char **argv)
{
  if (!rmrInit(argv)) {
    return;
  }
  if (argc != 4) {
    responsePrintError(argv[RMR_CI_RESPONSE], 0x83, "Incorrect number of arguments");
    return;
  }
  uint8_t structure = ciGetUnsigned(argv[1]);
  uint16_t location = ciGetUnsigned(argv[2]);
  uint8_t pointer = ciGetUnsigned(argv[3]);
  if (Rmr_updateConfigurationPointer(structure, location, pointer) != RAIL_STATUS_NO_ERROR) {
    responsePrintError(argv[RMR_CI_RESPONSE], 0x84, "Error updating structure");
    return;
  }
  responsePrint(argv[RMR_CI_RESPONSE], "CommandStatus:Success");
}

void CI_reconfigureModem(int argc, char **argv)
{
  if (!rmrInit(argv)) {
    return;
  }
  if (Rmr_reconfigureModem(railHandle) == RAIL_STATUS_NO_ERROR) {
    responsePrint(argv[RMR_CI_RESPONSE], "CommandStatus:Success");
  } else {
    responsePrintError(argv[RMR_CI_RESPONSE], 0x85, "Need to be in Idle radio state for this command");
  }
}
