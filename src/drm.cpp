// DRM output stuff

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/select.h>

extern "C" {
#include <wlr/types/wlr_buffer.h>
}

#include "cvt.hpp"
#include "drm.hpp"
#include "main.hpp"
#include "vblankmanager.hpp"
#include "wlserver.hpp"

#include "gpuvis_trace_utils.h"

#include <algorithm>
#include <thread>

struct drm_t g_DRM = {};

uint32_t g_nDRMFormat = DRM_FORMAT_INVALID;
bool g_bRotated = false;

bool g_bUseLayers = true;
bool g_bDebugLayers = false;

static int s_drm_log = 0;

static struct crtc *find_crtc_for_encoder(struct drm_t *drm, const drmModeEncoder *encoder) {
	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		/* possible_crtcs is a bitmask as described here:
		 * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
		 */
		uint32_t crtc_mask = 1 << i;
		if (encoder->possible_crtcs & crtc_mask)
			return &drm->crtcs[i];
	}

	/* no match found */
	return nullptr;
}

static struct crtc *find_crtc_for_connector(struct drm_t *drm, const drmModeConnector *connector) {
	for (int i = 0; i < connector->count_encoders; i++) {
		uint32_t encoder_id = connector->encoders[i];

		drmModeEncoder *encoder = drmModeGetEncoder(drm->fd, encoder_id);
		if (encoder == nullptr)
			continue;

		struct crtc *crtc = find_crtc_for_encoder(drm, encoder);
		drmModeFreeEncoder(encoder);
		if (crtc != nullptr)
			return crtc;
	}

	/* no match found */
	return nullptr;
}

#define MAX_DRM_DEVICES 64

static int find_drm_device(void)
{
	drmDevicePtr devices[MAX_DRM_DEVICES] = { NULL };
	int num_devices, fd = -1;

	num_devices = drmGetDevices2(0, devices, MAX_DRM_DEVICES);
	if (num_devices < 0) {
		perror("drmGetDevices2 failed");
		return -1;
	}

	for (int i = 0; i < num_devices; i++) {
		drmDevicePtr device = devices[i];

		if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
			continue;

		fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (drmIsKMS(fd))
			break;

		close(fd);
		fd = -1;
	}

	drmFreeDevices(devices, num_devices);

	if (fd < 0)
		fprintf(stderr, "no drm device found!\n");
	return fd;
}

static bool get_plane_formats(struct drm_t *drm, struct plane *plane, struct wlr_drm_format_set *formats) {
	for (uint32_t k = 0; k < plane->plane->count_formats; k++) {
		uint32_t fmt = plane->plane->formats[k];
		wlr_drm_format_set_add(formats, fmt, DRM_FORMAT_MOD_INVALID);
	}

	if (plane->props.count("IN_FORMATS") > 0) {
		uint64_t blob_id = plane->initial_prop_values["IN_FORMATS"];

		drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(drm->fd, blob_id);
		if (!blob) {
			perror("drmModeGetPropertyBlob(IN_FORMATS) failed");
			return false;
		}

		struct drm_format_modifier_blob *data =
			(struct drm_format_modifier_blob *)blob->data;
		uint32_t *fmts = (uint32_t *)((char *)data + data->formats_offset);
		struct drm_format_modifier *mods = (struct drm_format_modifier *)
			((char *)data + data->modifiers_offset);
		for (uint32_t i = 0; i < data->count_modifiers; ++i) {
			for (int j = 0; j < 64; ++j) {
				if (mods[i].formats & ((uint64_t)1 << j)) {
					wlr_drm_format_set_add(formats,
						fmts[j + mods[i].offset], mods[i].modifier);
				}
			}
		}

		drmModeFreePropertyBlob(blob);
	}

	return true;
}

static uint32_t pick_plane_format( const struct wlr_drm_format_set *formats )
{
	uint32_t result = DRM_FORMAT_INVALID;
	for ( size_t i = 0; i < formats->len; i++ ) {
		uint32_t fmt = formats->formats[i]->format;
		if ( fmt == DRM_FORMAT_XRGB8888 ) {
			// Prefer formats without alpha channel for main plane
			result = fmt;
		} else if ( result == DRM_FORMAT_INVALID && fmt == DRM_FORMAT_ARGB8888 ) {
			result = fmt;
		}
	}
	return result;
}

/* Pick a primary plane that can be connected to the chosen CRTC. */
static struct plane *find_primary_plane(struct drm_t *drm)
{
	struct plane *ret = nullptr;

	for (size_t i = 0; i < drm->planes.size() && ret == nullptr; i++) {
		struct plane *plane = &drm->planes[i];

		if (!(plane->plane->possible_crtcs & (1 << drm->crtc_index)))
			continue;

		if (!get_plane_formats(drm, plane, &drm->formats)) {
			return nullptr;
		}

		uint64_t plane_type = drm->planes[i].initial_prop_values["type"];
		if (plane_type == DRM_PLANE_TYPE_PRIMARY) {
			/* found our primary plane, let's use that */
			ret = plane;

			if (!get_plane_formats(drm, plane, &drm->plane_formats)) {
				return nullptr;
			}
		}
	}

	if ( ret == nullptr )
		return nullptr;

	g_nDRMFormat = pick_plane_format( &drm->plane_formats );
	if ( g_nDRMFormat == DRM_FORMAT_INVALID ) {
		fprintf( stderr, "Primary plane doesn't support XRGB8888 nor ARGB8888\n" );
		return nullptr;
	}

	return ret;
}

static void drm_free_fb( struct drm_t *drm, struct fb *fb );

static void page_flip_handler(int fd, unsigned int frame,
							  unsigned int sec, unsigned int usec, void *data)
{
	vblank_mark_possible_vblank();

	// TODO: get the fbids_queued instance from data if we ever have more than one in flight

	if ( s_drm_log != 0 )
	{
		fprintf(stderr, "page_flip_handler %p\n", data);
	}
	gpuvis_trace_printf("page_flip_handler %p", data);

	for ( uint32_t i = 0; i < g_DRM.fbids_on_screen.size(); i++ )
	{
		uint32_t previous_fbid = g_DRM.fbids_on_screen[ i ];
		assert( previous_fbid != 0 );
		assert( g_DRM.map_fbid_inflightflips[ previous_fbid ].n_refs > 0 );

		g_DRM.map_fbid_inflightflips[ previous_fbid ].n_refs--;

		if ( g_DRM.map_fbid_inflightflips[ previous_fbid ].n_refs == 0 )
		{
			// we flipped away from this previous fbid, now safe to delete
			std::lock_guard<std::mutex> lock( g_DRM.free_queue_lock );

			for ( uint32_t i = 0; i < g_DRM.fbid_free_queue.size(); i++ )
			{
				if ( g_DRM.fbid_free_queue[ i ] == previous_fbid )
				{
					if ( s_drm_log != 0 )
					{
						fprintf(stderr, "deferred free %u\n", previous_fbid);
					}
					drm_free_fb( &g_DRM, &g_DRM.map_fbid_inflightflips[ previous_fbid ] );

					g_DRM.fbid_free_queue.erase( g_DRM.fbid_free_queue.begin() + i );
					break;
				}
			}
		}
	}

	g_DRM.fbids_on_screen = g_DRM.fbids_queued;
	g_DRM.fbids_queued.clear();

	g_DRM.flip_lock.unlock();
}

void flip_handler_thread_run(void)
{
	fd_set fds;
	int ret;
	drmEventContext evctx = {
		.version = 2,
		.page_flip_handler = page_flip_handler,
	};

	FD_ZERO(&fds);
	FD_SET(0, &fds);
	FD_SET(g_DRM.fd, &fds);

	while ( true )
	{
		ret = select(g_DRM.fd + 1, &fds, NULL, NULL, NULL);
		if (ret < 0) {
			break;
		}
		drmHandleEvent(g_DRM.fd, &evctx);
	}
}

static bool get_properties(struct drm_t *drm, uint32_t obj_id, uint32_t obj_type, std::map<std::string, drmModePropertyRes *> &map, std::map<std::string, uint64_t> &values)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(drm->fd, obj_id, obj_type);
	if (!props) {
		perror("drmModeObjectGetProperties failed");
		return false;
	}

	map = {};
	values = {};

	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *prop = drmModeGetProperty(drm->fd, props->props[i]);
		if (!prop) {
			perror("drmModeGetProperty failed");
			return false;
		}
		map[prop->name] = prop;
		values[prop->name] = props->prop_values[i];
	}

	drmModeFreeObjectProperties(props);
	return true;
}

static bool get_resources(struct drm_t *drm)
{
	drmModeRes *resources = drmModeGetResources(drm->fd);
	if (resources == nullptr) {
		perror("drmModeGetResources failed");
		return false;
	}

	for (int i = 0; i < resources->count_connectors; i++) {
		struct connector conn = { .id = resources->connectors[i] };

		conn.connector = drmModeGetConnector(drm->fd, conn.id);
		if (conn.connector == nullptr) {
			perror("drmModeGetConnector failed");
			return false;
		}

		if (!get_properties(drm, conn.id, DRM_MODE_OBJECT_CONNECTOR, conn.props, conn.initial_prop_values)) {
			return false;
		}

		drm->connectors.push_back(conn);
	}

	for (int i = 0; i < resources->count_crtcs; i++) {
		struct crtc crtc = { .id = resources->crtcs[i] };

		crtc.crtc = drmModeGetCrtc(drm->fd, crtc.id);
		if (crtc.crtc == nullptr) {
			perror("drmModeGetCrtc failed");
			return false;
		}

		if (!get_properties(drm, crtc.id, DRM_MODE_OBJECT_CRTC, crtc.props, crtc.initial_prop_values)) {
			return false;
		}

		drm->crtcs.push_back(crtc);
	}

	drmModeFreeResources(resources);

	drmModePlaneRes *plane_resources = drmModeGetPlaneResources(drm->fd);
	if (!plane_resources) {
		perror("drmModeGetPlaneResources failed");
		return false;
	}

	for (uint32_t i = 0; i < plane_resources->count_planes; i++) {
		struct plane plane = { .id = plane_resources->planes[i] };

		plane.plane = drmModeGetPlane(drm->fd, plane.id);
		if (plane.plane == nullptr) {
			perror("drmModeGetPlane failed");
			return false;
		}

		if (!get_properties(drm, plane.id, DRM_MODE_OBJECT_PLANE, plane.props, plane.initial_prop_values)) {
			return false;
		}

		drm->planes.push_back(plane);
	}

	drmModeFreePlaneResources(plane_resources);

	return true;
}

static const drmModeModeInfo *get_matching_mode( const drmModeConnector *connector, int hdisplay, int vdisplay, uint32_t vrefresh )
{
	for (int i = 0; i < connector->count_modes; i++) {
		const drmModeModeInfo *mode = &connector->modes[i];

		if (hdisplay != 0 && hdisplay != mode->hdisplay)
			continue;
		if (vdisplay != 0 && vdisplay != mode->vdisplay)
			continue;
		if (vrefresh != 0 && vrefresh != mode->vrefresh)
			continue;

		return mode;
	}

	return NULL;
}

static bool compare_modes( drmModeModeInfo mode1, drmModeModeInfo mode2 )
{
	if (mode1.type & DRM_MODE_TYPE_PREFERRED)
		return true;
	if (mode2.type & DRM_MODE_TYPE_PREFERRED)
		return false;

	int area1 = mode1.hdisplay * mode1.vdisplay;
	int area2 = mode2.hdisplay * mode2.vdisplay;
	if (area1 != area2)
		return area1 > area2;

	return mode1.vrefresh > mode2.vrefresh;
}

int init_drm(struct drm_t *drm, const char *device)
{
	drmModeConnector *connector = NULL;
	int ret;

	if (device) {
		drm->fd = open(device, O_RDWR | O_CLOEXEC);
		if (!drmIsKMS(drm->fd)) {
			fprintf(stderr, "%s is not a KMS device\n", device);
			return -1;
		}
	} else {
		drm->fd = find_drm_device();
	}

	if (drm->fd < 0) {
		fprintf(stderr, "could not open drm device\n");
		return -1;
	}

	if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
		fprintf(stderr, "drmSetClientCap(ATOMIC) failed\n");
		return -1;
	}

	if (drmGetCap(drm->fd, DRM_CAP_CURSOR_WIDTH, &drm->cursor_width) != 0) {
		drm->cursor_width = 64;
	}
	if (drmGetCap(drm->fd, DRM_CAP_CURSOR_HEIGHT, &drm->cursor_height) != 0) {
		drm->cursor_height = 64;
	}

	uint64_t cap;
	if (drmGetCap(drm->fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap != 0) {
		drm->allow_modifiers = true;
	}

	if (!get_resources(drm)) {
		return -1;
	}

	/* find a connected connector */
	for (size_t i = 0; i < drm->connectors.size(); i++) {
		if (drm->connectors[i].connector->connection == DRM_MODE_CONNECTED) {
			drm->connector = &drm->connectors[i];
			connector = drm->connector->connector;
			break;
		}
	}

	if (!drm->connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		fprintf(stderr, "no connected connector!\n");
		return -1;
	}

	/* sort modes by preference: preferred flag, then highest area, then
	 * highest refresh rate */
	std::stable_sort(connector->modes, connector->modes + connector->count_modes, compare_modes);

	const drmModeModeInfo *mode = nullptr;
	if ( g_nOutputWidth != 0 || g_nOutputHeight != 0 || g_nNestedRefresh != 0 )
	{
		mode = get_matching_mode(connector, g_nOutputWidth, g_nOutputHeight, g_nNestedRefresh);
	}

	if (!mode) {
		mode = get_matching_mode(connector, 0, 0, 0);
	}

	if (!mode) {
		fprintf(stderr, "could not find mode!\n");
		return -1;
	}

	if (!drm_set_mode(drm, mode)) {
		fprintf(stderr, "failed to set initial mode\n");
		return -1;
	}

	drm->crtc = find_crtc_for_connector(drm, drm->connector->connector);
	if (drm->crtc == nullptr) {
		fprintf(stderr, "no crtc found!\n");
		return -1;
	}

	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		if (drm->crtcs[i].id == drm->crtc->id) {
			drm->crtc_index = i;
			break;
		}
	}

	// Disable all CRTCs. This ensures the CRTC we've selected isn't being used
	// by another connector.
	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		ret = drmModeSetCrtc(drm->fd, drm->crtcs[i].id, 0, 0, 0, nullptr, 0, nullptr);
		if (ret != 0)
			fprintf(stderr, "failed to disable CRTC %" PRIu32 ": %s", drm->crtcs[i].id, strerror(-ret));
	}

	drm->plane = find_primary_plane( drm );
	if ( drm->plane == nullptr ) {
		fprintf(stderr, "could not find a suitable primary plane\n");
		return -1;
	}

	drm->kms_in_fence_fd = -1;

	std::thread flip_handler_thread( flip_handler_thread_run );
	flip_handler_thread.detach();

	if (g_bUseLayers) {
		liftoff_log_set_priority(g_bDebugLayers ? LIFTOFF_DEBUG : LIFTOFF_ERROR);
	}

	drm->lo_device = liftoff_device_create( drm->fd );
	drm->lo_output = liftoff_output_create( drm->lo_device, drm->crtc->id );

	liftoff_device_register_all_planes( drm->lo_device );

	assert( drm->lo_device && drm->lo_output );

	for ( int i = 0; i < k_nMaxLayers; i++ )
	{
		drm->lo_layers[ i ] = liftoff_layer_create( drm->lo_output );
		assert( drm->lo_layers[ i ] );
	}

	drm->flipcount = 0;

	return 0;
}

static int add_connector_property(struct drm_t *drm, drmModeAtomicReq *req,
								  struct connector *conn, const char *name,
								  uint64_t value)
{
	if ( conn->props.count( name ) == 0 )
	{
		fprintf(stderr, "no connector property: %s\n", name);
		return -EINVAL;
	}

	const drmModePropertyRes *prop = conn->props[ name ];

	int ret = drmModeAtomicAddProperty(req, conn->id, prop->prop_id, value);
	if ( ret < 0 )
	{
		perror( "drmModeAtomicAddProperty failed" );
	}
	return ret;
}

static int add_crtc_property(struct drm_t *drm, drmModeAtomicReq *req,
							 struct crtc *crtc, const char *name,
							 uint64_t value)
{
	if ( crtc->props.count( name ) == 0 )
	{
		fprintf(stderr, "no CRTC property: %s\n", name);
		return -EINVAL;
	}

	const drmModePropertyRes *prop = crtc->props[ name ];

	int ret = drmModeAtomicAddProperty(req, crtc->id, prop->prop_id, value);
	if ( ret < 0 )
	{
		perror( "drmModeAtomicAddProperty failed" );
	}
	return ret;
}

static int add_plane_property(struct drm_t *drm, drmModeAtomicReq *req,
							  struct plane *plane, const char *name, uint64_t value)
{
	if ( plane->props.count( name ) == 0 )
	{
		fprintf(stderr, "no plane property: %s\n", name);
		return -EINVAL;
	}

	const drmModePropertyRes *prop = plane->props[ name ];

	int ret = drmModeAtomicAddProperty(req, plane->id, prop->prop_id, value);
	if ( ret < 0 )
	{
		perror( "drmModeAtomicAddProperty failed" );
	}
	return ret;
}

int drm_atomic_commit(struct drm_t *drm, struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline )
{
	int ret;

	assert( drm->req != nullptr );

// 	if (drm->kms_in_fence_fd != -1) {
// 		add_plane_property(drm, req, plane_id, "IN_FENCE_FD", drm->kms_in_fence_fd);
// 	}

// 	drm->kms_out_fence_fd = -1;

// 	add_crtc_property(drm, req, drm->crtc_id, "OUT_FENCE_PTR",
// 					  (uint64_t)(unsigned long)&drm->kms_out_fence_fd);

	if ( s_drm_log != 0 )
	{
		fprintf(stderr, "flipping\n");
	}
	drm->flip_lock.lock();

	// Do it before the commit, as otherwise the pageflip handler could
	// potentially beat us to the refcount checks.
	for ( uint32_t i = 0; i < drm->fbids_in_req.size(); i++ )
	{
		assert( g_DRM.map_fbid_inflightflips[ drm->fbids_in_req[ i ] ].held == true );
		g_DRM.map_fbid_inflightflips[ drm->fbids_in_req[ i ] ].n_refs++;
	}

	assert( drm->fbids_queued.size() == 0 );
	drm->fbids_queued = drm->fbids_in_req;

	g_DRM.flipcount++;
	gpuvis_trace_printf ( "legacy flip commit %lu", (uint64_t)g_DRM.flipcount );
	ret = drmModeAtomicCommit(drm->fd, drm->req, drm->flags, (void*)(uint64_t)g_DRM.flipcount );
	if (ret)
	{
		if ( ret != -EBUSY && ret != -ENOSPC )
		{
			fprintf( stderr, "flip error %d\n", ret);
			exit( 1 );
		}
		else
		{
			fprintf( stderr, "flip busy %d\n", ret);
		}

		// Undo refcount if the commit didn't actually work
		for ( uint32_t i = 0; i < drm->fbids_in_req.size(); i++ )
		{
			g_DRM.map_fbid_inflightflips[ drm->fbids_in_req[ i ] ].n_refs--;
		}

		drm->fbids_queued.clear();

		g_DRM.flipcount--;

		drm->flip_lock.unlock();

		goto out;
	} else {
		drm->fbids_in_req.clear();

		if ( drm->flags & DRM_MODE_ATOMIC_ALLOW_MODESET )
		{
			drm->mode = drm->pending.mode;
			drm->mode_id = drm->pending.mode_id;
		}
	}

	// Wait for flip handler to unlock
	drm->flip_lock.lock();
	drm->flip_lock.unlock();

// 	if (drm->kms_in_fence_fd != -1) {
// 		close(drm->kms_in_fence_fd);
// 		drm->kms_in_fence_fd = -1;
// 	}
//
// 	drm->kms_in_fence_fd = drm->kms_out_fence_fd;

out:
	drmModeAtomicFree( drm->req );
	drm->req = nullptr;

	return ret;
}

uint32_t drm_fbid_from_dmabuf( struct drm_t *drm, struct wlr_buffer *buf, struct wlr_dmabuf_attributes *dma_buf )
{
	uint32_t fb_id = 0;

	if ( !wlr_drm_format_set_has( &drm->formats, dma_buf->format, dma_buf->modifier ) )
	{
		if ( s_drm_log != 0 )
		{
			fprintf( stderr, "Cannot import FB to DRM: format 0x%" PRIX32 " and modifier 0x%" PRIX64 " not supported for scan-out\n", dma_buf->format, dma_buf->modifier );
		}
		return 0;
	}

	uint32_t handles[4] = {0};
	uint64_t modifiers[4] = {0};
	for ( int i = 0; i < dma_buf->n_planes; i++ ) {
		if ( drmPrimeFDToHandle( drm->fd, dma_buf->fd[i], &handles[i] ) != 0 )
		{
			perror("drmPrimeFDToHandle failed");
			return 0;
		}

		/* KMS requires all planes to have the same modifier */
		modifiers[i] = dma_buf->modifier;
	}

	if ( dma_buf->modifier != DRM_FORMAT_MOD_INVALID )
	{
		if ( !drm->allow_modifiers )
		{
			fprintf(stderr, "Cannot import DMA-BUF: has a modifier (0x%" PRIX64 "), but KMS doesn't support them\n", dma_buf->modifier);
			return 0;
		}

		if ( drmModeAddFB2WithModifiers( drm->fd, dma_buf->width, dma_buf->height, dma_buf->format, handles, dma_buf->stride, dma_buf->offset, modifiers, &fb_id, DRM_MODE_FB_MODIFIERS ) != 0 )
		{
			perror("drmModeAddFB2WithModifiers failed");
			return 0;
		}
	}
	else
	{
		if ( drmModeAddFB2( drm->fd, dma_buf->width, dma_buf->height, dma_buf->format, handles, dma_buf->stride, dma_buf->offset, &fb_id, 0 ) != 0 )
		{
			perror("drmModeAddFB2 failed");
			return 0;
		}
	}

	if ( s_drm_log != 0 )
	{
		fprintf(stderr, "make fbid %u\n", fb_id);
	}
	assert( drm->map_fbid_inflightflips[ fb_id ].held == false );

	if ( buf != nullptr )
	{
		wlserver_lock();
		buf = wlr_buffer_lock( buf );
		wlserver_unlock();
	}

	drm->map_fbid_inflightflips[ fb_id ].id = fb_id;
	drm->map_fbid_inflightflips[ fb_id ].buf = buf;
	drm->map_fbid_inflightflips[ fb_id ].held = true;
	drm->map_fbid_inflightflips[ fb_id ].n_refs = 0;

	return fb_id;
}

static void drm_free_fb( struct drm_t *drm, struct fb *fb )
{
	assert( !fb->held );
	assert( fb->n_refs == 0 );

	if ( drmModeRmFB( drm->fd, fb->id ) != 0 )
	{
		perror( "drmModeRmFB failed" );
	}

	if ( fb->buf != nullptr )
	{
		wlserver_lock();
		wlr_buffer_unlock( fb->buf );
		wlserver_unlock();
	}

	fb = {};
}

void drm_drop_fbid( struct drm_t *drm, uint32_t fbid )
{
	assert( drm->map_fbid_inflightflips[ fbid ].held == true );
	drm->map_fbid_inflightflips[ fbid ].held = false;

	if ( drm->map_fbid_inflightflips[ fbid ].n_refs == 0 )
	{
		/* FB isn't being used in any page-flip, free it immediately */
		if ( s_drm_log != 0 )
		{
			fprintf(stderr, "free fbid %u\n", fbid);
		}
		drm_free_fb( drm, &drm->map_fbid_inflightflips[ fbid ] );
	}
	else
	{
		std::lock_guard<std::mutex> lock( drm->free_queue_lock );

		drm->fbid_free_queue.push_back( fbid );
	}
}

/* Prepares an atomic commit without using libliftoff */
static bool
drm_prepare_basic( struct drm_t *drm, const struct Composite_t *pComposite, const struct VulkanPipeline_t *pPipeline )
{
	// Discard cases where our non-liftoff path is known to fail

	// It only supports one layer
	if ( pComposite->nLayerCount > 1 )
	{
		return false;
	}

	if ( pPipeline->layerBindings[ 0 ].fbid == 0 )
	{
		return false;
	}

	drmModeAtomicReq *req = drm->req;
	uint32_t fb_id = pPipeline->layerBindings[ 0 ].fbid;

	drm->fbids_in_req.push_back( fb_id );

	if ( g_bRotated )
	{
		add_plane_property(drm, req, drm->plane, "rotation", DRM_MODE_ROTATE_270);
	}

	add_plane_property(drm, req, drm->plane, "FB_ID", fb_id);
	add_plane_property(drm, req, drm->plane, "CRTC_ID", drm->crtc->id);
	add_plane_property(drm, req, drm->plane, "SRC_X", 0);
	add_plane_property(drm, req, drm->plane, "SRC_Y", 0);
	add_plane_property(drm, req, drm->plane, "SRC_W", pPipeline->layerBindings[ 0 ].surfaceWidth << 16);
	add_plane_property(drm, req, drm->plane, "SRC_H", pPipeline->layerBindings[ 0 ].surfaceHeight << 16);

	gpuvis_trace_printf ( "legacy flip fb_id %u src %ix%i", fb_id,
						 pPipeline->layerBindings[ 0 ].surfaceWidth,
						 pPipeline->layerBindings[ 0 ].surfaceHeight );

	int64_t crtcX = pComposite->data.vOffset[ 0 ].x * -1;
	int64_t crtcY = pComposite->data.vOffset[ 0 ].y * -1;
	int64_t crtcW = pPipeline->layerBindings[ 0 ].surfaceWidth / pComposite->data.vScale[ 0 ].x;
	int64_t crtcH = pPipeline->layerBindings[ 0 ].surfaceHeight / pComposite->data.vScale[ 0 ].y;

	if ( g_bRotated )
	{
		int64_t tmp = crtcX;
		crtcX = crtcY;
		crtcY = tmp;

		tmp = crtcW;
		crtcW = crtcH;
		crtcH = tmp;
	}

	add_plane_property(drm, req, drm->plane, "CRTC_X", crtcX);
	add_plane_property(drm, req, drm->plane, "CRTC_Y", crtcY);
	add_plane_property(drm, req, drm->plane, "CRTC_W", crtcW);
	add_plane_property(drm, req, drm->plane, "CRTC_H", crtcH);

	gpuvis_trace_printf ( "crtc %li,%li %lix%li", crtcX, crtcY, crtcW, crtcH );

	unsigned test_flags = (drm->flags & DRM_MODE_ATOMIC_ALLOW_MODESET) | DRM_MODE_ATOMIC_TEST_ONLY;
	int ret = drmModeAtomicCommit( drm->fd, drm->req, test_flags, NULL );

	if ( ret != 0 && ret != -EINVAL && ret != -ERANGE ) {
		fprintf( stderr, "drmModeAtomicCommit failed: %s", strerror( -ret ) );
	}

	return ret == 0;
}

static bool
drm_prepare_liftoff( struct drm_t *drm, const struct Composite_t *pComposite, const struct VulkanPipeline_t *pPipeline )
{
	for ( int i = 0; i < k_nMaxLayers; i++ )
	{
		if ( i < pComposite->nLayerCount )
		{
			if ( pPipeline->layerBindings[ i ].fbid == 0 )
			{
				return false;
			}

			liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", pPipeline->layerBindings[ i ].fbid);
			drm->fbids_in_req.push_back( pPipeline->layerBindings[ i ].fbid );

			liftoff_layer_set_property( drm->lo_layers[ i ], "zpos", pPipeline->layerBindings[ i ].zpos );
			liftoff_layer_set_property( drm->lo_layers[ i ], "alpha", pComposite->data.flOpacity[ i ] * 0xffff);

			if ( pPipeline->layerBindings[ i ].zpos == 0 )
			{
				assert( ( pComposite->data.flOpacity[ i ] * 0xffff ) == 0xffff );
			}

			const uint16_t srcWidth = pPipeline->layerBindings[ i ].surfaceWidth;
			const uint16_t srcHeight = pPipeline->layerBindings[ i ].surfaceHeight;

			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_X", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_Y", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_W", srcWidth << 16);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_H", srcHeight << 16);

			int32_t crtcX = -pComposite->data.vOffset[ i ].x;
			int32_t crtcY = -pComposite->data.vOffset[ i ].y;
			uint64_t crtcW = srcWidth / pComposite->data.vScale[ i ].x;
			uint64_t crtcH = srcHeight / pComposite->data.vScale[ i ].y;

			if (g_bRotated) {
				const int32_t x = crtcX;
				const uint64_t w = crtcW;
				crtcX = crtcY;
				crtcY = x;
				crtcW = crtcH;
				crtcH = w;

				liftoff_layer_set_property( drm->lo_layers[ i ], "rotation", DRM_MODE_ROTATE_270);
			}

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_X", crtcX);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_Y", crtcY);

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_W", crtcW);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_H", crtcH);
		}
		else
		{
			liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", 0 );
		}
	}

	bool ret = liftoff_output_apply( drm->lo_output, drm->req, drm->flags );

	int scanoutLayerCount = 0;
	if ( ret )
	{
		for ( int i = 0; i < k_nMaxLayers; i++ )
		{
			if ( liftoff_layer_get_plane( drm->lo_layers[ i ] ) != NULL )
				scanoutLayerCount++;
		}
		ret = scanoutLayerCount == pComposite->nLayerCount;
	}

	if ( s_drm_log != 0 )
	{
		if ( ret )
			fprintf( stderr, "can drm present %i layers\n", pComposite->nLayerCount );
		else
			fprintf( stderr, "can NOT drm present %i layers\n", pComposite->nLayerCount );
	}

	return ret;
}

/* Prepares an atomic commit for the provided scene-graph. Returns false on
 * error or if the scene-graph can't be presented directly. */
bool drm_prepare( struct drm_t *drm, const struct Composite_t *pComposite, const struct VulkanPipeline_t *pPipeline )
{
	drm->fbids_in_req.clear();

	assert( drm->req == nullptr );
	drm->req = drmModeAtomicAlloc();

	uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;

	// We do internal refcounting with these events
	flags |= DRM_MODE_PAGE_FLIP_EVENT;

	if ( drm->pending.mode_id != drm->mode_id ) {
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

		if (add_connector_property(drm, drm->req, drm->connector, "CRTC_ID", drm->crtc->id) < 0)
			return false;

		if (add_crtc_property(drm, drm->req, drm->crtc, "MODE_ID", drm->pending.mode_id) < 0)
			return false;

		if (add_crtc_property(drm, drm->req, drm->crtc, "ACTIVE", 1) < 0)
			return false;
	}

	drm->flags = flags;

	bool result;
	if ( g_bUseLayers == true ) {
		result = drm_prepare_liftoff( drm, pComposite, pPipeline );
	} else {
		result = drm_prepare_basic( drm, pComposite, pPipeline );
	}

	if ( !result ) {
		drmModeAtomicFree( drm->req );
		drm->req = nullptr;

		drm->fbids_in_req.clear();
	}

	return result;
}

bool drm_set_mode( struct drm_t *drm, const drmModeModeInfo *mode )
{
	if (drmModeCreatePropertyBlob(drm->fd, mode, sizeof(*mode), &drm->pending.mode_id) != 0)
		return false;

	drm->pending.mode = *mode;
	fprintf( stderr, "drm: selecting mode %dx%d@%uHz\n", mode->hdisplay, mode->vdisplay, mode->vrefresh );

	g_nOutputWidth = mode->hdisplay;
	g_nOutputHeight = mode->vdisplay;
	g_nOutputRefresh = mode->vrefresh;

	if ( g_nOutputWidth < g_nOutputHeight )
	{
		// We probably don't want to be in portrait mode, rotate
		g_bRotated = true;
	}

	if ( g_bRotated )
	{
		g_nOutputWidth = mode->vdisplay;
		g_nOutputHeight = mode->hdisplay;
	}

	return true;
}

bool drm_set_refresh( struct drm_t *drm, int refresh )
{
	drmModeConnector *connector = drm->connector->connector;
	const drmModeModeInfo *existing_mode = get_matching_mode(connector, g_nOutputWidth, g_nOutputHeight, refresh);
	drmModeModeInfo mode;
	if ( existing_mode )
	{
		mode = *existing_mode;
	}
	else
	{
		/* TODO: check refresh is within the EDID limits */
		generate_cvt_mode( &mode, g_nOutputWidth, g_nOutputHeight, refresh, true, false );
	}

	return drm_set_mode(drm, &mode);
}
