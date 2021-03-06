/**

  Temporary BS and RT overrides for boot.efi support.
  Unlike RtShims they do not affect the kernel.

  by dmazar

**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "Config.h"
#include "ServiceOverrides.h"
#include "BootArgs.h"
#include "BootFixes.h"
#include "CustomSlide.h"
#include "Hibernate.h"
#include "MemoryMap.h"
#include "RtShims.h"
#include "UmmMalloc/UmmMalloc.h"

//
// Placeholders for storing original Boot and RT Services functions
//
STATIC EFI_ALLOCATE_PAGES          mStoredAllocatePages;
STATIC EFI_ALLOCATE_POOL           mStoredAllocatePool;
STATIC EFI_FREE_POOL               mStoredFreePool;
STATIC EFI_GET_MEMORY_MAP          mStoredGetMemoryMap;
STATIC EFI_EXIT_BOOT_SERVICES      mStoredExitBootServices;
STATIC EFI_HANDLE_PROTOCOL         mStoredHandleProtocol;
STATIC EFI_SET_VIRTUAL_ADDRESS_MAP mStoredSetVirtualAddressMap;

//
// Original runtime services hash we restore on uninstallation
//
STATIC UINT32               mRtPreOverridesCRC32;

//
// Location of memory allocated by boot.efi for hibernate image
//
STATIC EFI_PHYSICAL_ADDRESS mHibernateImageAddress;

//
// Saved exit boot services arguments
//
STATIC EFI_HANDLE           mExitBSImageHandle;
STATIC UINTN                mExitBSMapKey;

//
// Dynamic memory allocation filtering status
//
STATIC BOOLEAN              mFilterDynamicPoolAllocations;

//
// Minimum and maximum addresses allocated by AlocatePages
//
EFI_PHYSICAL_ADDRESS        gMinAllocatedAddr;
EFI_PHYSICAL_ADDRESS        gMaxAllocatedAddr;

//
// Last descriptor size obtained from GetMemoryMap
//
UINTN                       gMemoryMapDescriptorSize = sizeof(EFI_MEMORY_DESCRIPTOR);

VOID
InstallBsOverrides (
  VOID
  )
{
#if APTIOFIX_CUSTOM_POOL_ALLOCATOR == 1
  EFI_STATUS               Status;
  EFI_PHYSICAL_ADDRESS     UmmHeap = BASE_4GB;
  UINTN                    PageNum = EFI_SIZE_TO_PAGES (APTIOFIX_CUSTOM_POOL_ALLOCATOR_SIZE);

  //
  // We do not uninstall our custom allocator to avoid memory corruption issues
  // when shimming AMI code. This check ensures that we do not install it twice.
  // See UninstallBsOverrides for more details.
  //
  if (!UmmInitialized ()) {
    //
    // Enforce memory pool creation when -aptiodump argument is used, but let it slip otherwise.
    //
    Status = AllocatePagesFromTop (EfiBootServicesData, PageNum, &UmmHeap, !gDumpMemArgPresent);
    if (!EFI_ERROR (Status)) {
      gBS->SetMem ((VOID *)UmmHeap, APTIOFIX_CUSTOM_POOL_ALLOCATOR_SIZE, 0);
      UmmSetHeap ((VOID *)UmmHeap);

      mStoredAllocatePool   = gBS->AllocatePool;
      mStoredFreePool       = gBS->FreePool;

      gBS->AllocatePool     = MOAllocatePool;
      gBS->FreePool         = MOFreePool;
    } else {
      //
      // This is undesired, but technically less fatal than attempting to reduce the number
      // of slides available when no memory map dumping is necessary, for example.
      //
      PrintScreen (L"AMF: Not using custom memory pool - %r\n", Status);
    }
  }
#endif

  mStoredAllocatePages    = gBS->AllocatePages;
  mStoredGetMemoryMap     = gBS->GetMemoryMap;
  mStoredExitBootServices = gBS->ExitBootServices;
  mStoredHandleProtocol   = gBS->HandleProtocol;

  gBS->AllocatePages      = MOAllocatePages;
  gBS->GetMemoryMap       = MOGetMemoryMap;
  gBS->ExitBootServices   = MOExitBootServices;
  gBS->HandleProtocol     = MOHandleProtocol;

  gBS->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
}

VOID
UninstallBsOverrides (
  VOID
  )
{
  //
  // AllocatePool and FreePool restoration is intentionally not present!
  // Uninstalling a custom allocator is unsafe if anything tries to free
  // an allocated pointer afterwards. While this should not be the case for
  // our code, there are no guarantees for AMI, which we have to shim.
  // See MOAllocatePool itself for bug details.
  //

  gBS->AllocatePages    = mStoredAllocatePages;
  gBS->GetMemoryMap     = mStoredGetMemoryMap;
  gBS->ExitBootServices = mStoredExitBootServices;
  gBS->HandleProtocol   = mStoredHandleProtocol;

  gBS->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
}

VOID
InstallRtOverrides (
  VOID
  )
{
  mRtPreOverridesCRC32 = gRT->Hdr.CRC32;

  mStoredSetVirtualAddressMap = gRT->SetVirtualAddressMap;

  gRT->SetVirtualAddressMap = MOSetVirtualAddressMap;

  gRT->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gRT, gRT->Hdr.HeaderSize, &gRT->Hdr.CRC32);
}

VOID
UninstallRtOverrides (
  VOID
  )
{
  gRT->SetVirtualAddressMap = mStoredSetVirtualAddressMap;

  gRT->Hdr.CRC32 = mRtPreOverridesCRC32;
}

VOID
DisableDynamicPoolAllocations (
  VOID
  )
{
  mFilterDynamicPoolAllocations = TRUE;
}

VOID
EnableDynamicPoolAllocations (
  VOID
  )
{
  mFilterDynamicPoolAllocations = FALSE;
}

/** gBS->HandleProtocol override:
 * Boot.efi requires EfiGraphicsOutputProtocol on ConOutHandle, but it is not present
 * there on Aptio 2.0. EfiGraphicsOutputProtocol exists on some other handle.
 * If this is the case, we'll intercept that call and return EfiGraphicsOutputProtocol
 * from that other handle.
 */
EFI_STATUS
EFIAPI
MOHandleProtocol (
  IN     EFI_HANDLE  Handle,
  IN     EFI_GUID    *Protocol,
     OUT VOID        **Interface
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *GraphicsOutput;

  Status = mStoredHandleProtocol (Handle, Protocol, Interface);

  if (EFI_ERROR (Status) && CompareGuid (Protocol, &gEfiGraphicsOutputProtocolGuid)) {
    //
    // Let's find it on some other handle
    //
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&GraphicsOutput);
    if (Status == EFI_SUCCESS) {
      *Interface = GraphicsOutput;
      DEBUG ((DEBUG_VERBOSE, "HandleProtocol(%p, %g, %p) = %r (from other handle)\n", Handle, Protocol, *Interface, Status));
    }
  }

  return Status;
}

/** gBS->AllocatePages override:
 * Returns pages from free memory block to boot.efi for kernel boot image.
 */
EFI_STATUS
EFIAPI
MOAllocatePages (
  IN     EFI_ALLOCATE_TYPE     Type,
  IN     EFI_MEMORY_TYPE       MemoryType,
  IN     UINTN                 NumberOfPages,
  IN OUT EFI_PHYSICAL_ADDRESS  *Memory
  )
{
  EFI_STATUS              Status;
  EFI_PHYSICAL_ADDRESS    UpperAddr;

  if (Type == AllocateAddress && MemoryType == EfiLoaderData) {
    //
    // Called from boot.efi
    //
    UpperAddr = *Memory + EFI_PAGES_TO_SIZE (NumberOfPages);

    //
    // Store min and max mem - can be used later to determine start and end of kernel boot image
    //
    if (gMinAllocatedAddr == 0 || *Memory < gMinAllocatedAddr)
      gMinAllocatedAddr = *Memory;
    if (UpperAddr > gMaxAllocatedAddr)
      gMaxAllocatedAddr = UpperAddr;

    Status = mStoredAllocatePages (Type, MemoryType, NumberOfPages, Memory);
  } else if (gHibernateWake && Type == AllocateAnyPages && MemoryType == EfiLoaderData) {
    //
    // Called from boot.efi during hibernate wake,
    // first such allocation is for hibernate image
    //
    Status = mStoredAllocatePages (Type, MemoryType, NumberOfPages, Memory);
    if (mHibernateImageAddress == 0 && Status == EFI_SUCCESS) {
      mHibernateImageAddress = *Memory;
    }
  } else {
    //
    // Generic page allocation
    //
    Status = mStoredAllocatePages (Type, MemoryType, NumberOfPages, Memory);
  }

  return Status;
}

/** gBS->AllocatePool override:
 * Allows us to use a custom allocator that uses a preallocated memory pool
 * for certain types of memory. See details in PrintScreen function.
 */
EFI_STATUS
EFIAPI
MOAllocatePool (
  IN     EFI_MEMORY_TYPE  Type,
  IN     UINTN            Size,
     OUT VOID             **Buffer
  )
{
  //
  // The code below allows us to more safely invoke Boot Services to perform onscreen
  // printing when no memory map modifications (pool memory allocation) is allowed.
  // While it is imperfect design-wise, it works very well on many ASUS Skylake boards
  // when performing memory map dumps via -aptiodump.
  //
  if (Type == EfiBootServicesData && mFilterDynamicPoolAllocations) {
    *Buffer = UmmMalloc ((UINT32)Size);
    if (*Buffer)
      return EFI_SUCCESS;

    //
    // Dynamic pool allocations filtering should technically only be used when booting is more
    // important than not allocating the requested memory and failing to do something.
    // However, since we skip other types of allocations anyway, not falling back here to using
    // the default allocator may have its own consequences on other boards.
    //
  }

  return mStoredAllocatePool (Type, Size, Buffer);
}

/** gBS->FreePool override:
 * Allows us to use a custom allocator for certain types of memory.
 */
EFI_STATUS
EFIAPI
MOFreePool (
  IN VOID  *Buffer
  )
{
  //
  // By default it will return FALSE if Buffer was not allocated by us.
  //
  if (UmmFree (Buffer))
    return EFI_SUCCESS;

  return mStoredFreePool (Buffer);
}

/** gBS->GetMemoryMap override:
 * Returns shrinked memory map. XNU can handle up to PMAP_MEMORY_REGIONS_SIZE (128) entries.
 */
EFI_STATUS
EFIAPI
MOGetMemoryMap (
  IN OUT UINTN                  *MemoryMapSize,
  IN OUT EFI_MEMORY_DESCRIPTOR  *MemoryMap,
     OUT UINTN                  *MapKey,
     OUT UINTN                  *DescriptorSize,
     OUT UINT32                 *DescriptorVersion
  )
{
  EFI_STATUS            Status;

  Status = mStoredGetMemoryMap (MemoryMapSize, MemoryMap, MapKey, DescriptorSize, DescriptorVersion);

  if (Status == EFI_SUCCESS) {
    if (gDumpMemArgPresent) {
      PrintMemMap (L"GetMemoryMap", *MemoryMapSize, *DescriptorSize, MemoryMap, gRtShims, gSysTableRtArea);
    }

#if APTIOFIX_PROTECT_CSM_REGION == 1
    ProtectCsmRegion (*MemoryMapSize, MemoryMap, *DescriptorSize);
#endif

    ShrinkMemMap (MemoryMapSize, MemoryMap, *DescriptorSize);

    //
    // Remember some descriptor size, since we will not have it later
    // during hibernate wake to be able to iterate memory map.
    //
    gMemoryMapDescriptorSize = *DescriptorSize;
  }

  return Status;
}

EFI_STATUS
EFIAPI
OrgGetMemoryMap (
  IN OUT UINTN                  *MemoryMapSize,
  IN OUT EFI_MEMORY_DESCRIPTOR  *MemoryMap,
     OUT UINTN                  *MapKey,
     OUT UINTN                  *DescriptorSize,
     OUT UINT32                 *DescriptorVersion
  )
{
  return (mStoredGetMemoryMap ? mStoredGetMemoryMap : gBS->GetMemoryMap) (
    MemoryMapSize,
    MemoryMap,
    MapKey,
    DescriptorSize,
    DescriptorVersion
    );
}

/** gBS->ExitBootServices override:
 * Patches kernel entry point with jump to our KernelEntryPatchJumpBack().
 */
EFI_STATUS
EFIAPI
MOExitBootServices (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  EFI_STATUS               Status;
  UINTN                    SlideAddr = 0;
  VOID                     *MachOImage = NULL;
  IOHibernateImageHeader   *ImageHeader = NULL;

  //
  // We need hibernate image address for wake
  //
  if (gHibernateWake && mHibernateImageAddress == 0) {
    PrintScreen (L"AMF: Failed to find hibernate image address\n");
    gBS->Stall (SECONDS_TO_MICROSECONDS (5));
    return EFI_INVALID_PARAMETER;
  }

  //
  // We can just return EFI_SUCCESS and continue using Print for debug
  //
  if (gDumpMemArgPresent) {
    mExitBSImageHandle = ImageHandle;
    mExitBSMapKey      = MapKey; 
    Status             = EFI_SUCCESS;
  } else {
    Status = ForceExitBootServices (mStoredExitBootServices, ImageHandle, MapKey);
  }

  DEBUG ((DEBUG_VERBOSE, "ExitBootServices %r\n", Status));

  if (EFI_ERROR (Status))
    return Status;

  if (!gHibernateWake) {
    DEBUG ((DEBUG_VERBOSE, "ExitBootServices: gMinAllocatedAddr: %lx, gMaxAllocatedAddr: %lx\n", gMinAllocatedAddr, gMaxAllocatedAddr));

    SlideAddr  = gMinAllocatedAddr - 0x100000;
    MachOImage = (VOID*)(UINTN)(SlideAddr + SLIDE_GRANULARITY);
    KernelEntryFromMachOPatchJump (MachOImage, SlideAddr);
  } else {
    //
    // At this stage HIB section is not yet copied from sleep image to it's
    // proper memory destination. so we'll patch entry point in sleep image.
    //
    ImageHeader = (IOHibernateImageHeader *)(UINTN)mHibernateImageAddress;
    KernelEntryPatchJump (
      ((UINT32)(UINTN)&(ImageHeader->fileExtentMap[0])) + ImageHeader->fileExtentMapSize + ImageHeader->restore1CodeOffset
      );
  }

  return Status;
}

/** Helper function to call ExitBootServices that can handle outdated MapKey issues. */
EFI_STATUS
ForceExitBootServices (
  IN EFI_EXIT_BOOT_SERVICES  ExitBs,
  IN EFI_HANDLE              ImageHandle,
  IN UINTN                   MapKey
  )
{
  EFI_STATUS               Status;
  EFI_MEMORY_DESCRIPTOR    *MemoryMap;
  UINTN                    MemoryMapSize;
  UINTN                    DescriptorSize;
  UINT32                   DescriptorVersion;

  //
  // Firstly try the easy way
  //
  Status = ExitBs (ImageHandle, MapKey);

  if (EFI_ERROR (Status)) {
    //
    // Just report error as var in nvram to be visible from macOS with "nvram -p"
    //
    gRT->SetVariable (L"aptiomemfix-exitbs",
      &gAppleBootVariableGuid,
      EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
      4,
      "fail"
      );

    //
    // It is too late to free memory map here, but it does not matter, because boot.efi has an old one
    // and will freely use the memory.
    // It is technically forbidden to allocate pool memory here, but we should not hit this code
    // in the first place, and for older firmwares, where it was necessary (?), it worked just fine.
    //
    Status = GetMemoryMapAlloc (NULL, &MemoryMapSize, &MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    DEBUG ((DEBUG_VERBOSE, "ExitBootServices: GetMemoryMapKey = %r\n", Status));
    if (Status == EFI_SUCCESS) {
      //
      // We have the latest memory map and its key, try again!
      //
      Status = ExitBs (ImageHandle, MapKey);
      DEBUG ((DEBUG_VERBOSE, "ExitBootServices: 2nd try = %r\n", Status));
      if (EFI_ERROR (Status))
        PrintScreen (L"AMF: ExitBootServices failed twice - %r\n", Status);
    } else {
      PrintScreen (L"AMF: Failed to get MapKey for ExitBootServices - %r\n", Status);
      Status = EFI_INVALID_PARAMETER;
    }

    if (EFI_ERROR (Status)) {
      PrintScreen (L"Waiting 10 secs...\n");
      gBS->Stall (SECONDS_TO_MICROSECONDS (10));
    }
  }

  return Status;
}

/** gRT->SetVirtualAddressMap override:
 * Fixes virtualizing of RT services.
 */
EFI_STATUS
EFIAPI
MOSetVirtualAddressMap (
  IN UINTN                  MemoryMapSize,
  IN UINTN                  DescriptorSize,
  IN UINT32                 DescriptorVersion,
  IN EFI_MEMORY_DESCRIPTOR  *VirtualMap
  )
{
  EFI_STATUS   Status;
  UINT32       EfiSystemTable;

  //
  // We do not need to recover BS, since they will be invalid.
  //
  UninstallRtOverrides ();

  if (gDumpMemArgPresent) {
    PrintMemMap (L"SetVirtualAddressMap", MemoryMapSize, DescriptorSize, VirtualMap, gRtShims, gSysTableRtArea);
    //
    // To print as much information as possible we delay ExitBootServices.
    // Most likely this will fail, but let's still try!
    //
    ForceExitBootServices (mStoredExitBootServices, mExitBSImageHandle, mExitBSMapKey);
  }

  //
  // Protect RT areas from relocation by marking then MemMapIO
  //
  ProtectRtMemoryFromRelocation (MemoryMapSize, DescriptorSize, DescriptorVersion, VirtualMap, gSysTableRtArea);

  //
  // Remember physical sys table addr
  //
  EfiSystemTable = (UINT32)(UINTN)gST;

  //
  // Virtualize RT services with all needed fixes
  //
  Status = ExecSetVirtualAddressesToMemMap (MemoryMapSize, DescriptorSize, DescriptorVersion, VirtualMap);

  CopyEfiSysTableToRtArea (&EfiSystemTable);

  //
  // Correct shim pointers right away
  //
  VirtualizeRtShims (MemoryMapSize, DescriptorSize, VirtualMap);

  return Status;
}
