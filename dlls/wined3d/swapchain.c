/*
 *IDirect3DSwapChain9 implementation
 *
 *Copyright 2002-2003 Jason Edmeades
 *Copyright 2002-2003 Raphael Junqueira
 *Copyright 2005 Oliver Stieber
 *Copyright 2007-2008 Stefan Dösinger for CodeWeavers
 *
 *This library is free software; you can redistribute it and/or
 *modify it under the terms of the GNU Lesser General Public
 *License as published by the Free Software Foundation; either
 *version 2.1 of the License, or (at your option) any later version.
 *
 *This library is distributed in the hope that it will be useful,
 *but WITHOUT ANY WARRANTY; without even the implied warranty of
 *MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *Lesser General Public License for more details.
 *
 *You should have received a copy of the GNU Lesser General Public
 *License along with this library; if not, write to the Free Software
 *Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wined3d_private.h"


/*TODO: some of the additional parameters may be required to
    set the gamma ramp (for some weird reason microsoft have left swap gammaramp in device
    but it operates on a swapchain, it may be a good idea to move it to IWineD3DSwapChain for IWineD3D)*/


WINE_DEFAULT_DEBUG_CHANNEL(d3d);
WINE_DECLARE_DEBUG_CHANNEL(fps);

#define GLINFO_LOCATION This->wineD3DDevice->adapter->gl_info

/*IWineD3DSwapChain parts follow: */
static void WINAPI IWineD3DSwapChainImpl_Destroy(IWineD3DSwapChain *iface)
{
    IWineD3DSwapChainImpl *This = (IWineD3DSwapChainImpl *)iface;
    WINED3DDISPLAYMODE mode;
    unsigned int i;

    TRACE("Destroying swapchain %p\n", iface);

    IWineD3DSwapChain_SetGammaRamp(iface, 0, &This->orig_gamma);

    /* Release the swapchain's draw buffers. Make sure This->backBuffer[0] is
     * the last buffer to be destroyed, FindContext() depends on that. */
    if (This->frontBuffer)
    {
        IWineD3DSurface_SetContainer(This->frontBuffer, 0);
        if (IWineD3DSurface_Release(This->frontBuffer))
        {
            WARN("(%p) Something's still holding the front buffer (%p).\n",
                    This, This->frontBuffer);
        }
        This->frontBuffer = NULL;
    }

    if (This->backBuffer)
    {
        UINT i = This->presentParms.BackBufferCount;

        while (i--)
        {
            IWineD3DSurface_SetContainer(This->backBuffer[i], 0);
            if (IWineD3DSurface_Release(This->backBuffer[i]))
                WARN("(%p) Something's still holding back buffer %u (%p).\n",
                        This, i, This->backBuffer[i]);
        }
        HeapFree(GetProcessHeap(), 0, This->backBuffer);
        This->backBuffer = NULL;
    }

    for (i = 0; i < This->num_contexts; ++i)
    {
        context_destroy(This->wineD3DDevice, This->context[i]);
    }
    /* Restore the screen resolution if we rendered in fullscreen
     * This will restore the screen resolution to what it was before creating the swapchain. In case of d3d8 and d3d9
     * this will be the original desktop resolution. In case of d3d7 this will be a NOP because ddraw sets the resolution
     * before starting up Direct3D, thus orig_width and orig_height will be equal to the modes in the presentation params
     */
    if(This->presentParms.Windowed == FALSE && This->presentParms.AutoRestoreDisplayMode) {
        mode.Width = This->orig_width;
        mode.Height = This->orig_height;
        mode.RefreshRate = 0;
        mode.Format = This->orig_fmt;
        IWineD3DDevice_SetDisplayMode((IWineD3DDevice *) This->wineD3DDevice, 0, &mode);
    }
    HeapFree(GetProcessHeap(), 0, This->context);

    HeapFree(GetProcessHeap(), 0, This);
}

/* A GL context is provided by the caller */
static inline void swapchain_blit(IWineD3DSwapChainImpl *This, struct wined3d_context *context)
{
    RECT window;
    IWineD3DDeviceImpl *device = This->wineD3DDevice;
    IWineD3DSurfaceImpl *backbuffer = ((IWineD3DSurfaceImpl *) This->backBuffer[0]);
    UINT w = backbuffer->currentDesc.Width;
    UINT h = backbuffer->currentDesc.Height;
    GLenum gl_filter;
    const struct wined3d_gl_info *gl_info = context->gl_info;

    GetClientRect(This->win_handle, &window);
    if(w == window.right && h == window.bottom) gl_filter = GL_NEAREST;
    else gl_filter = GL_LINEAR;

    if(gl_info->supported[EXT_FRAMEBUFFER_BLIT])
    {
        ENTER_GL();
        context_bind_fbo(context, GL_READ_FRAMEBUFFER, &context->src_fbo);
        context_attach_surface_fbo(context, GL_READ_FRAMEBUFFER, 0, This->backBuffer[0]);
        context_attach_depth_stencil_fbo(context, GL_READ_FRAMEBUFFER, NULL, FALSE);

        context_bind_fbo(context, GL_DRAW_FRAMEBUFFER, NULL);
        glDrawBuffer(GL_BACK);

        glDisable(GL_SCISSOR_TEST);
        IWineD3DDeviceImpl_MarkStateDirty(This->wineD3DDevice, STATE_RENDER(WINED3DRS_SCISSORTESTENABLE));

        /* Note that the texture is upside down */
        gl_info->fbo_ops.glBlitFramebuffer(0, 0, w, h,
                                           window.left, window.bottom, window.right, window.top,
                                           GL_COLOR_BUFFER_BIT, gl_filter);
        checkGLcall("Swapchain present blit(EXT_framebuffer_blit)\n");
        LEAVE_GL();
    }
    else
    {
        struct wined3d_context *context2;
        float tex_left = 0;
        float tex_top = 0;
        float tex_right = w;
        float tex_bottom = h;

        context2 = context_acquire(This->wineD3DDevice, This->backBuffer[0], CTXUSAGE_BLIT);

        if(backbuffer->Flags & SFLAG_NORMCOORD)
        {
            tex_left /= w;
            tex_right /= w;
            tex_top /= h;
            tex_bottom /= h;
        }

        ENTER_GL();
        context_bind_fbo(context2, GL_DRAW_FRAMEBUFFER, NULL);

        /* Set up the texture. The surface is not in a IWineD3D*Texture container,
         * so there are no d3d texture settings to dirtify
         */
        device->blitter->set_shader((IWineD3DDevice *) device, backbuffer->resource.format_desc,
                                    backbuffer->texture_target, backbuffer->pow2Width,
                                    backbuffer->pow2Height);
        glTexParameteri(backbuffer->texture_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(backbuffer->texture_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glDrawBuffer(GL_BACK);

        /* Set the viewport to the destination rectandle, disable any projection
         * transformation set up by CTXUSAGE_BLIT, and draw a (-1,-1)-(1,1) quad.
         *
         * Back up viewport and matrix to avoid breaking last_was_blit
         *
         * Note that CTXUSAGE_BLIT set up viewport and ortho to match the surface
         * size - we want the GL drawable(=window) size.
         */
        glPushAttrib(GL_VIEWPORT_BIT);
        glViewport(window.left, window.top, window.right, window.bottom);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();

        glBegin(GL_QUADS);
            /* bottom left */
            glTexCoord2f(tex_left, tex_bottom);
            glVertex2i(-1, -1);

            /* top left */
            glTexCoord2f(tex_left, tex_top);
            glVertex2i(-1, 1);

            /* top right */
            glTexCoord2f(tex_right, tex_top);
            glVertex2i(1, 1);

            /* bottom right */
            glTexCoord2f(tex_right, tex_bottom);
            glVertex2i(1, -1);
        glEnd();

        glPopMatrix();
        glPopAttrib();

        device->blitter->unset_shader((IWineD3DDevice *) device);
        checkGLcall("Swapchain present blit(manual)\n");
        LEAVE_GL();

        context_release(context2);
    }
}

static HRESULT WINAPI IWineD3DSwapChainImpl_Present(IWineD3DSwapChain *iface, CONST RECT *pSourceRect, CONST RECT *pDestRect, HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion, DWORD dwFlags) {
    IWineD3DSwapChainImpl *This = (IWineD3DSwapChainImpl *)iface;
    struct wined3d_context *context;
    unsigned int sync;
    int retval;

    context = context_acquire(This->wineD3DDevice, This->backBuffer[0], CTXUSAGE_RESOURCELOAD);

    /* Render the cursor onto the back buffer, using our nifty directdraw blitting code :-) */
    if(This->wineD3DDevice->bCursorVisible && This->wineD3DDevice->cursorTexture) {
        IWineD3DSurfaceImpl cursor;
        RECT destRect = {This->wineD3DDevice->xScreenSpace - This->wineD3DDevice->xHotSpot,
                         This->wineD3DDevice->yScreenSpace - This->wineD3DDevice->yHotSpot,
                         This->wineD3DDevice->xScreenSpace + This->wineD3DDevice->cursorWidth - This->wineD3DDevice->xHotSpot,
                         This->wineD3DDevice->yScreenSpace + This->wineD3DDevice->cursorHeight - This->wineD3DDevice->yHotSpot};
        TRACE("Rendering the cursor. Creating fake surface at %p\n", &cursor);
        /* Build a fake surface to call the Blitting code. It is not possible to use the interface passed by
         * the application because we are only supposed to copy the information out. Using a fake surface
         * allows to use the Blitting engine and avoid copying the whole texture -> render target blitting code.
         */
        memset(&cursor, 0, sizeof(cursor));
        cursor.lpVtbl = &IWineD3DSurface_Vtbl;
        cursor.resource.ref = 1;
        cursor.resource.wineD3DDevice = This->wineD3DDevice;
        cursor.resource.pool = WINED3DPOOL_SCRATCH;
        cursor.resource.format_desc =
                getFormatDescEntry(WINED3DFMT_B8G8R8A8_UNORM, &This->wineD3DDevice->adapter->gl_info);
        cursor.resource.resourceType = WINED3DRTYPE_SURFACE;
        cursor.texture_name = This->wineD3DDevice->cursorTexture;
        cursor.texture_target = GL_TEXTURE_2D;
        cursor.texture_level = 0;
        cursor.currentDesc.Width = This->wineD3DDevice->cursorWidth;
        cursor.currentDesc.Height = This->wineD3DDevice->cursorHeight;
        cursor.glRect.left = 0;
        cursor.glRect.top = 0;
        cursor.glRect.right = cursor.currentDesc.Width;
        cursor.glRect.bottom = cursor.currentDesc.Height;
        /* The cursor must have pow2 sizes */
        cursor.pow2Width = cursor.currentDesc.Width;
        cursor.pow2Height = cursor.currentDesc.Height;
        /* The surface is in the texture */
        cursor.Flags |= SFLAG_INTEXTURE;
        /* DDBLT_KEYSRC will cause BltOverride to enable the alpha test with GL_NOTEQUAL, 0.0,
         * which is exactly what we want :-)
         */
        if (This->presentParms.Windowed) {
            MapWindowPoints(NULL, This->win_handle, (LPPOINT)&destRect, 2);
        }
        IWineD3DSurface_Blt(This->backBuffer[0], &destRect, (IWineD3DSurface *)&cursor,
                NULL, WINEDDBLT_KEYSRC, NULL, WINED3DTEXF_POINT);
    }
    if(This->wineD3DDevice->logo_surface) {
        /* Blit the logo into the upper left corner of the drawable */
        IWineD3DSurface_BltFast(This->backBuffer[0], 0, 0, This->wineD3DDevice->logo_surface, NULL, WINEDDBLTFAST_SRCCOLORKEY);
    }

    if (pSourceRect || pDestRect) FIXME("Unhandled present rects %s/%s\n", wine_dbgstr_rect(pSourceRect), wine_dbgstr_rect(pDestRect));
    /* TODO: If only source rect or dest rect are supplied then clip the window to match */
    TRACE("presetting HDC %p\n", This->context[0]->hdc);

    /* Don't call checkGLcall, as glGetError is not applicable here */
    if (hDestWindowOverride && This->win_handle != hDestWindowOverride) {
        IWineD3DSwapChain_SetDestWindowOverride(iface, hDestWindowOverride);
    }

    if(This->render_to_fbo)
    {
        /* This codepath should only be hit with the COPY swapeffect. Otherwise a backbuffer-
         * window size mismatch is impossible(fullscreen) and src and dst rectangles are
         * not allowed(they need the COPY swapeffect)
         *
         * The DISCARD swap effect is ok as well since any backbuffer content is allowed after
         * the swap
         */
        if(This->presentParms.SwapEffect == WINED3DSWAPEFFECT_FLIP )
        {
            FIXME("Render-to-fbo with WINED3DSWAPEFFECT_FLIP\n");
        }
        swapchain_blit(This, context);
    }

    SwapBuffers(This->context[0]->hdc); /* TODO: cycle through the swapchain buffers */

    TRACE("SwapBuffers called, Starting new frame\n");
    /* FPS support */
    if (TRACE_ON(fps))
    {
        DWORD time = GetTickCount();
        This->frames++;
        /* every 1.5 seconds */
        if (time - This->prev_time > 1500) {
            TRACE_(fps)("%p @ approx %.2ffps\n", This, 1000.0*This->frames/(time - This->prev_time));
            This->prev_time = time;
            This->frames = 0;
        }
    }

#if defined(FRAME_DEBUGGING)
{
    if (GetFileAttributesA("C:\\D3DTRACE") != INVALID_FILE_ATTRIBUTES) {
        if (!isOn) {
            isOn = TRUE;
            FIXME("Enabling D3D Trace\n");
            __WINE_SET_DEBUGGING(__WINE_DBCL_TRACE, __wine_dbch_d3d, 1);
#if defined(SHOW_FRAME_MAKEUP)
            FIXME("Singe Frame snapshots Starting\n");
            isDumpingFrames = TRUE;
            ENTER_GL();
            glClear(GL_COLOR_BUFFER_BIT);
            LEAVE_GL();
#endif

#if defined(SINGLE_FRAME_DEBUGGING)
        } else {
#if defined(SHOW_FRAME_MAKEUP)
            FIXME("Singe Frame snapshots Finishing\n");
            isDumpingFrames = FALSE;
#endif
            FIXME("Singe Frame trace complete\n");
            DeleteFileA("C:\\D3DTRACE");
            __WINE_SET_DEBUGGING(__WINE_DBCL_TRACE, __wine_dbch_d3d, 0);
#endif
        }
    } else {
        if (isOn) {
            isOn = FALSE;
#if defined(SHOW_FRAME_MAKEUP)
            FIXME("Single Frame snapshots Finishing\n");
            isDumpingFrames = FALSE;
#endif
            FIXME("Disabling D3D Trace\n");
            __WINE_SET_DEBUGGING(__WINE_DBCL_TRACE, __wine_dbch_d3d, 0);
        }
    }
}
#endif

    /* This is disabled, but the code left in for debug purposes.
     *
     * Since we're allowed to modify the new back buffer on a D3DSWAPEFFECT_DISCARD flip,
     * we can clear it with some ugly color to make bad drawing visible and ease debugging.
     * The Debug runtime does the same on Windows. However, a few games do not redraw the
     * screen properly, like Max Payne 2, which leaves a few pixels undefined.
     *
     * Tests show that the content of the back buffer after a discard flip is indeed not
     * reliable, so no game can depend on the exact content. However, it resembles the
     * old contents in some way, for example by showing fragments at other locations. In
     * general, the color theme is still intact. So Max payne, which draws rather dark scenes
     * gets a dark background image. If we clear it with a bright ugly color, the game's
     * bug shows up much more than it does on Windows, and the players see single pixels
     * with wrong colors.
     * (The Max Payne bug has been confirmed on Windows with the debug runtime)
     */
    if (FALSE && This->presentParms.SwapEffect == WINED3DSWAPEFFECT_DISCARD) {
        TRACE("Clearing the color buffer with cyan color\n");

        IWineD3DDevice_Clear((IWineD3DDevice*)This->wineD3DDevice, 0, NULL,
                WINED3DCLEAR_TARGET, 0xff00ffff, 1.0f, 0);
    }

    if(!This->render_to_fbo &&
       ( ((IWineD3DSurfaceImpl *) This->frontBuffer)->Flags   & SFLAG_INSYSMEM ||
         ((IWineD3DSurfaceImpl *) This->backBuffer[0])->Flags & SFLAG_INSYSMEM ) ) {
        /* Both memory copies of the surfaces are ok, flip them around too instead of dirtifying
         * Doesn't work with render_to_fbo because we're not flipping
         */
        IWineD3DSurfaceImpl *front = (IWineD3DSurfaceImpl *) This->frontBuffer;
        IWineD3DSurfaceImpl *back = (IWineD3DSurfaceImpl *) This->backBuffer[0];

        if(front->resource.size == back->resource.size) {
            DWORD fbflags;
            flip_surface(front, back);

            /* Tell the front buffer surface that is has been modified. However,
             * the other locations were preserved during that, so keep the flags.
             * This serves to update the emulated overlay, if any
             */
            fbflags = front->Flags;
            IWineD3DSurface_ModifyLocation(This->frontBuffer, SFLAG_INDRAWABLE, TRUE);
            front->Flags = fbflags;
        } else {
            IWineD3DSurface_ModifyLocation((IWineD3DSurface *) front, SFLAG_INDRAWABLE, TRUE);
            IWineD3DSurface_ModifyLocation((IWineD3DSurface *) back, SFLAG_INDRAWABLE, TRUE);
        }
    } else {
        IWineD3DSurface_ModifyLocation(This->frontBuffer, SFLAG_INDRAWABLE, TRUE);
        /* If the swapeffect is DISCARD, the back buffer is undefined. That means the SYSMEM
         * and INTEXTURE copies can keep their old content if they have any defined content.
         * If the swapeffect is COPY, the content remains the same. If it is FLIP however,
         * the texture / sysmem copy needs to be reloaded from the drawable
         */
        if(This->presentParms.SwapEffect == WINED3DSWAPEFFECT_FLIP) {
            IWineD3DSurface_ModifyLocation(This->backBuffer[0], SFLAG_INDRAWABLE, TRUE);
        }
    }

    if (This->wineD3DDevice->stencilBufferTarget) {
        if (This->presentParms.Flags & WINED3DPRESENTFLAG_DISCARD_DEPTHSTENCIL
                || ((IWineD3DSurfaceImpl *)This->wineD3DDevice->stencilBufferTarget)->Flags & SFLAG_DISCARD) {
            surface_modify_ds_location(This->wineD3DDevice->stencilBufferTarget, SFLAG_DS_DISCARDED);
        }
    }

    if (This->presentParms.PresentationInterval != WINED3DPRESENT_INTERVAL_IMMEDIATE
            && context->gl_info->supported[SGI_VIDEO_SYNC])
    {
        retval = GL_EXTCALL(glXGetVideoSyncSGI(&sync));
        if(retval != 0) {
            ERR("glXGetVideoSyncSGI failed(retval = %d\n", retval);
        }

        switch(This->presentParms.PresentationInterval) {
            case WINED3DPRESENT_INTERVAL_DEFAULT:
            case WINED3DPRESENT_INTERVAL_ONE:
                if(sync <= This->vSyncCounter) {
                    retval = GL_EXTCALL(glXWaitVideoSyncSGI(1, 0, &This->vSyncCounter));
                } else {
                    This->vSyncCounter = sync;
                }
                break;
            case WINED3DPRESENT_INTERVAL_TWO:
                if(sync <= This->vSyncCounter + 1) {
                    retval = GL_EXTCALL(glXWaitVideoSyncSGI(2, This->vSyncCounter & 0x1, &This->vSyncCounter));
                } else {
                    This->vSyncCounter = sync;
                }
                break;
            case WINED3DPRESENT_INTERVAL_THREE:
                if(sync <= This->vSyncCounter + 2) {
                    retval = GL_EXTCALL(glXWaitVideoSyncSGI(3, This->vSyncCounter % 0x3, &This->vSyncCounter));
                } else {
                    This->vSyncCounter = sync;
                }
                break;
            case WINED3DPRESENT_INTERVAL_FOUR:
                if(sync <= This->vSyncCounter + 3) {
                    retval = GL_EXTCALL(glXWaitVideoSyncSGI(4, This->vSyncCounter & 0x3, &This->vSyncCounter));
                } else {
                    This->vSyncCounter = sync;
                }
                break;
            default:
                FIXME("Unknown presentation interval %08x\n", This->presentParms.PresentationInterval);
        }
    }

    context_release(context);

    TRACE("returning\n");
    return WINED3D_OK;
}

static HRESULT WINAPI IWineD3DSwapChainImpl_SetDestWindowOverride(IWineD3DSwapChain *iface, HWND window) {
    IWineD3DSwapChainImpl *This = (IWineD3DSwapChainImpl *)iface;
    WINED3DLOCKED_RECT r;
    BYTE *mem;

    if(window == This->win_handle) return WINED3D_OK;

    TRACE("Performing dest override of swapchain %p from window %p to %p\n", This, This->win_handle, window);
    if(This->context[0] == This->wineD3DDevice->contexts[0]) {
        /* The primary context 'owns' all the opengl resources. Destroying and recreating that context requires downloading
         * all opengl resources, deleting the gl resources, destroying all other contexts, then recreating all other contexts
         * and reload the resources
         */
        delete_opengl_contexts((IWineD3DDevice *) This->wineD3DDevice, iface);
        This->win_handle             = window;
        create_primary_opengl_context((IWineD3DDevice *) This->wineD3DDevice, iface);
    } else {
        This->win_handle             = window;

        /* The old back buffer has to be copied over to the new back buffer. A lockrect - switchcontext - unlockrect
         * would suffice in theory, but it is rather nasty and may cause troubles with future changes of the locking code
         * So lock read only, copy the surface out, then lock with the discard flag and write back
         */
        IWineD3DSurface_LockRect(This->backBuffer[0], &r, NULL, WINED3DLOCK_READONLY);
        mem = HeapAlloc(GetProcessHeap(), 0, r.Pitch * ((IWineD3DSurfaceImpl *) This->backBuffer[0])->currentDesc.Height);
        memcpy(mem, r.pBits, r.Pitch * ((IWineD3DSurfaceImpl *) This->backBuffer[0])->currentDesc.Height);
        IWineD3DSurface_UnlockRect(This->backBuffer[0]);

        context_destroy(This->wineD3DDevice, This->context[0]);
        This->context[0] = context_create(This->wineD3DDevice, (IWineD3DSurfaceImpl *)This->frontBuffer,
                This->win_handle, FALSE /* pbuffer */, &This->presentParms);
        context_release(This->context[0]);

        IWineD3DSurface_LockRect(This->backBuffer[0], &r, NULL, WINED3DLOCK_DISCARD);
        memcpy(r.pBits, mem, r.Pitch * ((IWineD3DSurfaceImpl *) This->backBuffer[0])->currentDesc.Height);
        HeapFree(GetProcessHeap(), 0, mem);
        IWineD3DSurface_UnlockRect(This->backBuffer[0]);
    }
    return WINED3D_OK;
}

const IWineD3DSwapChainVtbl IWineD3DSwapChain_Vtbl =
{
    /* IUnknown */
    IWineD3DBaseSwapChainImpl_QueryInterface,
    IWineD3DBaseSwapChainImpl_AddRef,
    IWineD3DBaseSwapChainImpl_Release,
    /* IWineD3DSwapChain */
    IWineD3DBaseSwapChainImpl_GetParent,
    IWineD3DSwapChainImpl_Destroy,
    IWineD3DBaseSwapChainImpl_GetDevice,
    IWineD3DSwapChainImpl_Present,
    IWineD3DSwapChainImpl_SetDestWindowOverride,
    IWineD3DBaseSwapChainImpl_GetFrontBufferData,
    IWineD3DBaseSwapChainImpl_GetBackBuffer,
    IWineD3DBaseSwapChainImpl_GetRasterStatus,
    IWineD3DBaseSwapChainImpl_GetDisplayMode,
    IWineD3DBaseSwapChainImpl_GetPresentParameters,
    IWineD3DBaseSwapChainImpl_SetGammaRamp,
    IWineD3DBaseSwapChainImpl_GetGammaRamp
};

struct wined3d_context *swapchain_create_context_for_thread(IWineD3DSwapChain *iface)
{
    IWineD3DSwapChainImpl *This = (IWineD3DSwapChainImpl *) iface;
    struct wined3d_context **newArray;
    struct wined3d_context *ctx;

    TRACE("Creating a new context for swapchain %p, thread %d\n", This, GetCurrentThreadId());

    ctx = context_create(This->wineD3DDevice, (IWineD3DSurfaceImpl *) This->frontBuffer,
            This->context[0]->win_handle, FALSE /* pbuffer */, &This->presentParms);
    if (!ctx)
    {
        ERR("Failed to create a new context for the swapchain\n");
        return NULL;
    }
    context_release(ctx);

    newArray = HeapAlloc(GetProcessHeap(), 0, sizeof(*newArray) * This->num_contexts + 1);
    if(!newArray) {
        ERR("Out of memory when trying to allocate a new context array\n");
        context_destroy(This->wineD3DDevice, ctx);
        return NULL;
    }
    memcpy(newArray, This->context, sizeof(*newArray) * This->num_contexts);
    HeapFree(GetProcessHeap(), 0, This->context);
    newArray[This->num_contexts] = ctx;
    This->context = newArray;
    This->num_contexts++;

    TRACE("Returning context %p\n", ctx);
    return ctx;
}

void get_drawable_size_swapchain(struct wined3d_context *context, UINT *width, UINT *height)
{
    IWineD3DSurfaceImpl *surface = (IWineD3DSurfaceImpl *)context->current_rt;
    /* The drawable size of an onscreen drawable is the surface size.
     * (Actually: The window size, but the surface is created in window size) */
    *width = surface->currentDesc.Width;
    *height = surface->currentDesc.Height;
}
