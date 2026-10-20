/** @file
  Read-only GIULIAESP FAT payload from the Android bootloader initrd.
  Block I/O callbacks adapted from UniXNU uefi-sheng ShengBootMemoryDxe.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  Does not add RAM or access UFS. Only accepts an initrd contained in the
  upstream giulia MemoryMapLib Kernel reservation [0xA8000000,0xB8000000).
*/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define RAM_BLOCK_SIZE  512

STATIC VOID Report (UINTN Stage, UINT64 Start, UINT64 End, EFI_STATUS Status)
{
  EFI_GUID Guid = {0x1c87b711,0xeaa6,0x42cb,{0xa3,0xd5,0x8c,0x21,0x19,0x65,0x21,0x31}};
  CHAR16 Text[128];
  UnicodeSPrint (Text, sizeof (Text), L"initrd S%u %lx-%lx %r", (UINT32)Stage, Start, End, Status);
  // Volatile only: no EFI_VARIABLE_NON_VOLATILE attribute.
  gRT->SetVariable (L"GiuliaInitrdStatus", &Guid,
      EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, StrSize (Text), Text);
}

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0xA9C6827D, 0x1BD8, 0x410C, { 0x88, 0x5A, 0xC4, 0x65, 0x00, 0xF7, 0xD8, 0x36 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8                    *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA       mMedia;

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

STATIC
UINT64
ReadAddress (IN CONST VOID *Property, IN INT32 Length)
{
  if (Length == 8) {
    return Fdt64ToCpu (ReadUnaligned64 ((CONST UINT64 *)Property));
  }
  return Fdt32ToCpu (ReadUnaligned32 ((CONST UINT32 *)Property));
}

EFI_STATUS
EFIAPI
GiuliaInitrdDxeEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HOB_GUID_TYPE *Hob;
  CONST VOID *Fdt;
  CONST VOID *StartProp;
  CONST VOID *EndProp;
  INT32 Chosen;
  INT32 StartLength;
  INT32 EndLength;
  UINT64 Start;
  UINT64 End;
  UINT64 At;
  UINT64 Bytes;
  UINT64 Dtb;
  UINT8 *Sector;
  EFI_HANDLE Handle;
  EFI_STATUS Status;

  Report (1, 0, 0, EFI_NOT_READY);
  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64))) {
    Report (2, 0, 0, EFI_NOT_FOUND);
    return EFI_NOT_FOUND;
  }
  Dtb = ReadUnaligned64 ((CONST UINT64 *)GET_GUID_HOB_DATA (Hob));
  if ((Dtb == 0) || ((Dtb & 3) != 0)) {
    Report (3, Dtb, 0, EFI_NOT_FOUND);
    return EFI_NOT_FOUND;
  }
  Fdt = (CONST VOID *)(UINTN)Dtb;
  if (FdtCheckHeader (Fdt) != 0) {
    Report (4, Dtb, 0, EFI_COMPROMISED_DATA);
    return EFI_COMPROMISED_DATA;
  }
  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    Report (5, Dtb, 0, EFI_NOT_FOUND);
    return EFI_NOT_FOUND;
  }
  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) ||
      ((EndLength != 4) && (EndLength != 8))) {
    Report (6, Dtb, 0, EFI_NOT_FOUND);
    return EFI_NOT_FOUND;
  }
  Start = ReadAddress (StartProp, StartLength);
  End = ReadAddress (EndProp, EndLength);
  // Restrict reads to giulia's already-reserved Kernel region. Do not guess
  // about an initrd outside it or add arbitrary physical memory to the GCD.
  if ((Start < 0xA8000000ULL) || (End > 0xB8000000ULL) ||
      (End <= Start) || ((End - Start) < RAM_BLOCK_SIZE)) {
    DEBUG ((DEBUG_ERROR, "GIULIAINITRD: rejected range 0x%lx-0x%lx\n", Start, End));
    Report (7, Start, End, EFI_UNSUPPORTED);
    return EFI_UNSUPPORTED;
  }
  // Android v4 loaders may prepend the vendor ramdisk. Search sector-aligned
  // offsets for our FAT32 volume rather than treating that prefix as a disk.
  for (At = Start; At <= End - RAM_BLOCK_SIZE; At += RAM_BLOCK_SIZE) {
    Sector = (UINT8 *)(UINTN)At;
    if ((Sector[510] != 0x55) || (Sector[511] != 0xAA) ||
        (ReadUnaligned16 ((CONST UINT16 *)(Sector + 11)) != RAM_BLOCK_SIZE) ||
        (CompareMem (Sector + 82, "FAT32   ", 8) != 0) ||
        (CompareMem (Sector + 71, "GIULIAESP  ", 11) != 0)) {
      continue;
    }
    Bytes = (UINT64)ReadUnaligned32 ((CONST UINT32 *)(Sector + 32)) * RAM_BLOCK_SIZE;
    if ((Bytes < SIZE_1MB) || (Bytes > End - At)) {
      continue;
    }
    mRamBase = Sector;
    mMedia.MediaId = 1;
    mMedia.MediaPresent = TRUE;
    mMedia.ReadOnly = TRUE;
    mMedia.BlockSize = RAM_BLOCK_SIZE;
    mMedia.IoAlign = 1;
    mMedia.LastBlock = Bytes / RAM_BLOCK_SIZE - 1;
    Handle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (&Handle,
        &gEfiDevicePathProtocolGuid, &mDevicePath,
        &gEfiBlockIoProtocolGuid, &mBlockIo, NULL);
    DEBUG ((DEBUG_ERROR, "GIULIAINITRD: FAT at 0x%lx size 0x%lx: %r\n", At, Bytes, Status));
    Report (9, At, At + Bytes, Status);
    return Status;
  }
  DEBUG ((DEBUG_ERROR, "GIULIAINITRD: no complete GIULIAESP FAT volume\n"));
  Report (8, Start, End, EFI_NOT_FOUND);
  return EFI_NOT_FOUND;
}

