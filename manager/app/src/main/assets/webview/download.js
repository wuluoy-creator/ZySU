(function() {
    if (window.ksu_download_enabled) return;
    window.ksu_download_enabled = true;

    const CHUNK_SIZE = 512 * 1024;
    const SIZE_THRESHOLD = 10 * 1024 * 1024;

    const blobMap = new Map();
    const originalCreateObjectURL = URL.createObjectURL;
    URL.createObjectURL = (obj) => {
        const url = originalCreateObjectURL(obj);
        if (obj instanceof Blob) blobMap.set(url, obj);
        return url;
    };

    const originalRevokeObjectURL = URL.revokeObjectURL;
    URL.revokeObjectURL = (url) => {
        setTimeout(() => blobMap.delete(url), 10000);
        return originalRevokeObjectURL(url);
    };

    const readChunk = (blob, start, end) => {
        return new Promise((resolve, reject) => {
            const chunk = blob.slice(start, end);
            const reader = new FileReader();
            reader.onload = () => resolve(reader.result.split(',')[1] || '');
            reader.onerror = () => reject(reader.error || new Error('Failed to read chunk'));
            reader.readAsDataURL(chunk);
        });
    };

    const downloadChunked = async (blob, fileName) => {
        const mimeType = blob.type || 'application/octet-stream';
        const downloadId = ksu_download.startChunkedDownload(fileName, mimeType);

        if (!downloadId) {
            throw new Error('Failed to start chunked download');
        }

        try {
            let offset = 0;
            const size = blob.size;

            while (offset < size) {
                const end = Math.min(offset + CHUNK_SIZE, size);
                const base64Chunk = await readChunk(blob, offset, end);

                if (!ksu_download.writeDownloadChunk(downloadId, base64Chunk)) {
                    throw new Error('Failed to write chunk');
                }

                offset = end;
            }

            if (!ksu_download.completeChunkedDownload(downloadId)) {
                throw new Error('Failed to complete download');
            }

        } catch (error) {
            ksu_download.cancelChunkedDownload(downloadId);
            throw error;
        }
    };

    const downloadLegacy = async (blob, fileName) => {
        const base64 = await new Promise((resolve, reject) => {
            const reader = new FileReader();
            reader.onload = () => resolve(reader.result.split(',')[1] || '');
            reader.onerror = () => reject(reader.error || new Error('Failed to read blob'));
            reader.readAsDataURL(blob);
        });
        ksu_download.save(base64, fileName, blob.type || null);
    };

    const handleDownload = async (anchor) => {
        const url = new URL(anchor.href, location.href);
        const fileName = anchor.download || url.pathname.split("/").pop().split("?")[0] || "download.bin";
        const isInternal = url.origin === location.origin;

        if (url.protocol === 'blob:' || url.protocol === 'data:' || isInternal) {
            let blob = url.protocol === 'blob:' ? blobMap.get(url.href) : null;
            if (!blob) {
                const response = await fetch(url.href, { credentials: 'include' });
                if (!response.ok) throw new Error(`Download failed: HTTP ${response.status}`);
                blob = await response.blob();
            }

            if (blob.size > SIZE_THRESHOLD) {
                await downloadChunked(blob, fileName);
            } else {
                await downloadLegacy(blob, fileName);
            }
            return;
        }

        ksu_download.download(url.href, fileName, null);
    };

    // Also handle downloads initiated without an anchor's download attribute.
    window.__ksu_download_url = (url, fileName) => handleDownload({ href: url, download: fileName || '' })
        .catch((error) => console.error('KernelSU download failed', error));

    document.addEventListener('click', (event) => {
        const anchor = event.target instanceof Element ? event.target.closest('a[download]') : null;
        if (!anchor || !anchor.href) return;
        event.preventDefault();
        handleDownload(anchor).catch((error) => console.error('KernelSU download failed', error));
    }, true);

    const originalClick = HTMLAnchorElement.prototype.click;
    HTMLAnchorElement.prototype.click = function() {
        if (this.hasAttribute('download') && this.href) {
            handleDownload(this).catch((error) => console.error('KernelSU download failed', error));
            return;
        }
        return originalClick.apply(this, arguments);
    };
})();
