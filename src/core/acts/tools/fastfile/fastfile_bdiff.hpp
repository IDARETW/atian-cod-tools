#pragma once
#include <core/bytebuffer.hpp>
#include <acts_api/fastfile.h>

namespace fastfile::bdiff {
    // used for the checksum
    using BDiffType = ActsAPIFastFile_BDiffType;
    using BDiffState = ActsAPIFastFile_BDiffState;

    using vcSourceCB_t = ActsAPIFastFile_sourceCallback;
    using vcDiffCB_t = ActsAPIFastFile_diffCallback;
    using vcDestCB_t = ActsAPIFastFile_destCallback;

    bool bdiff(BDiffState* state, vcSourceCB_t* sourceDataCB, vcDiffCB_t* patchDataCB, vcDestCB_t* destDataCB);

    /*
     * bdiff byte buffers
     * @param sourceData source buffer
     * @param patchData patch buffer
     * @param type bdiff type, used for checksum validation
     * @param winsize max window size, used to avoid reallocation
     */
    std::vector<byte> bdiff(
        core::bytebuffer::ByteBuffer* sourceData, core::bytebuffer::ByteBuffer* patchData, BDiffType type = BDT_UNKNOWN,
        size_t winsize = 0x100000
    );

    /*
     * bdiff in-memory buffers with the windows decoded and checksummed on several threads. A window only reads the
     * source file, its own output and the stored checksum of the window before it, so windows are independent once
     * their headers are scanned.
     * @param source source data
     * @param sourceLen source data size
     * @param patch patch data (the whole VCDIFF stream)
     * @param patchLen patch data size
     * @param type bdiff type, used for checksum validation
     * @param out patched data, allocated here without a fill pass
     * @param outLen patched data size
     * @return false when the patch does not have the plain shape this path handles (or a window fails its checks);
     *         nothing is returned in out then and bdiff() gives the reference result or error
     */
    bool bdiffParallel(
        const byte* source, size_t sourceLen, const byte* patch, size_t patchLen, BDiffType type,
        std::unique_ptr<byte[]>& out, size_t& outLen
    );
} // namespace fastfile::bdiff