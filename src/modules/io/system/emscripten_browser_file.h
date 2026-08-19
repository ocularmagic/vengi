#pragma once

#include <string>
#include <string_view>
#include <emscripten.h>
#include "app/App.h"
#include "core/Singleton.h"
#include "core/StringUtil.h"
#include "io/Filesystem.h"
#include "io/MemoryReadStream.h"
#include "video/EventHandler.h"

#define _EM_JS_INLINE(ret, c_name, js_name, params, code)                          \
  extern "C" {                                                                     \
  ret c_name params EM_IMPORT(js_name);                                            \
  EMSCRIPTEN_KEEPALIVE                                                             \
  __attribute__((section("em_js"), aligned(1))) inline char __em_js__##js_name[] = \
    #params "<::>" code;                                                           \
  }

#define EM_JS_INLINE(ret, name, params, ...) _EM_JS_INLINE(ret, name, name, params, #__VA_ARGS__)

namespace emscripten_browser_file {

/////////////////////////////////// Interface //////////////////////////////////

using upload_handler = void(*)(std::string const&, std::string const&, std::string_view buffer, void*);

inline void upload(std::string const &accept_types, upload_handler callback, void *callback_data = nullptr);
inline void download(std::string const &filename, std::string const &mime_type, std::string_view buffer, bool use_picker = true);

///////////////////////////////// Implementation ///////////////////////////////

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-variable-declarations"
EM_JS_INLINE(void, upload, (char const *accept_types, upload_handler callback, void *callback_data), {
  /// Prompt the browser to open the file selector dialogue, and pass the file to the given handler
  /// Accept-types are in the format ".png,.jpeg,.jpg" as per https://developer.mozilla.org/en-US/docs/Web/HTML/Attributes/accept
  /// Upload handler callback signature is:
  ///   void my_handler(std::string const &filename, std::string const &mime_type, std::string_view buffer, void *callback_data = nullptr);
  ///   Note: the string_view buffer is only valid for the duration of the callback - do not store it for later use.
  globalThis["open_file"] = function(e) {
    const file_reader = new FileReader();
    file_reader.onload = (event) => {
      const uint8Arr = new Uint8Array(event.target.result);
      const data_ptr = Module["_malloc"](uint8Arr.length);
      const data_on_heap = new Uint8Array(Module["HEAPU8"].buffer, data_ptr, uint8Arr.length);
      data_on_heap.set(uint8Arr);
      Module["ccall"]('upload_file_return', 'number', ['string', 'string', 'number', 'number', 'number', 'number'], [event.target.filename, event.target.mime_type, data_on_heap.byteOffset, uint8Arr.length, callback, callback_data]);
      Module["_free"](data_ptr);
    };
    file_reader.filename = e.target.files[0].name;
    file_reader.mime_type = e.target.files[0].type;
    file_reader.readAsArrayBuffer(e.target.files[0]);
  };
  var file_selector = document.createElement('input');
  file_selector.setAttribute('type', 'file');
  file_selector.setAttribute('onchange', 'globalThis["open_file"](event)');
  /// The 'cancel' event is fired when the user cancels the currently open dialog.
  /// In this case, the upload handler will get the empty string_view.
  /// See https://developer.mozilla.org/en-US/docs/Web/API/HTMLElement/cancel_event
  file_selector.addEventListener('cancel', () => {
    Module["ccall"]('upload_file_return', 'number', ['string', 'string', 'number', 'number', 'number', 'number'], ["", "", 0, 0, callback, callback_data]);
  });
  file_selector.setAttribute('accept', UTF8ToString(accept_types));
  /// file_selector.click() approach doesn't work in Safari (tested with native desktop v. 17.5 and iPhone/iPad simulators).
  /// It seems that the Safari browser limits programmatical clicking in our case.
  /// As a workaround, we create <dialog> where the user manually clicks on <input>.
  var is_safari = /^((?!chrome|android).)*safari/i.test(navigator.userAgent);
  if (is_safari) {
    var dialog = document.createElement('dialog');
    dialog.setAttribute('id', 'EmJsFileDialog');
    var desc = document.createElement('p');
    desc.innerText = 'Please choose a file. Allowed extension(s): ' + UTF8ToString(accept_types);
    dialog.appendChild(desc);
    /// We should recreate <dialog> every call; it is the most natural way to reset input.value.
    /// Otherwise, if the user re-selects the same file, it triggers the 'cancel' event instead of 'onchange'.
    file_selector.setAttribute('onclick', 'var dg = document.getElementById("EmJsFileDialog"); dg.close(); dg.remove()');
    dialog.appendChild(file_selector);
    document.body.append(dialog);
    dialog.showModal();
  } else {
    /// Not a Safari browser, so file_selector.click() is ok.
    file_selector.click();
  }
});
#pragma GCC diagnostic pop

inline void upload(std::string const &accept_types, upload_handler callback, void *callback_data) {
  /// C++ wrapper for javascript upload call
  upload(accept_types.c_str(), callback, callback_data);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-variable-declarations"
EM_JS_INLINE(void, download, (char const *filename, char const *mime_type, void const *buffer, size_t buffer_size, bool use_picker), {
  /// Offer a buffer in memory as a file to download, specifying download filename and mime type
  var name = UTF8ToString(filename);
  var type = UTF8ToString(mime_type);
  /// When HEAPU8 is backed by a SharedArrayBuffer (e.g. -pthread builds), the Blob constructor rejects it;
  /// slice() copies to a new non-shared ArrayBuffer first.  The typeof guard avoids a ReferenceError in
  /// environments where SharedArrayBuffer is not defined.
  var buffer_data = (typeof SharedArrayBuffer !== 'undefined' && Module["HEAPU8"].buffer instanceof SharedArrayBuffer)
    ? Module["HEAPU8"].slice(buffer, buffer + buffer_size)
    : new Uint8Array(Module["HEAPU8"].buffer, buffer, buffer_size);

  /// Try Modern File System Access API (window.showSaveFilePicker) if available and requested
  if (use_picker && typeof window.showSaveFilePicker === 'function') {
    (async function() {
      try {
        var ext = name.includes('.') ? '.' + name.split('.').pop().toLowerCase() : '.vengi';
        var saveTypes = [
          {
            description: 'Vengi Scene (*.vengi)',
            accept: { 'application/x-vengi': ['.vengi'] }
          },
          {
            description: 'MagicaVoxel (*.vox)',
            accept: { 'application/x-magicavoxel': ['.vox'] }
          },
          {
            description: 'Qubicle Binary (*.qb)',
            accept: { 'application/x-qubicle': ['.qb'] }
          },
          {
            description: 'Qubicle Binary Tree (*.qbt)',
            accept: { 'application/x-qubicle-tree': ['.qbt'] }
          },
          {
            description: 'Goxel (*.gox)',
            accept: { 'application/x-goxel': ['.gox'] }
          },
          {
            description: 'CubeWorld (*.cub)',
            accept: { 'application/x-cubeworld': ['.cub'] }
          },
          {
            description: 'BinVox (*.binvox)',
            accept: { 'application/x-binvox': ['.binvox'] }
          },
          {
            description: 'glTF Binary (*.glb)',
            accept: { 'model/gltf-binary': ['.glb'] }
          },
          {
            description: 'glTF JSON (*.gltf)',
            accept: { 'model/gltf+json': ['.gltf'] }
          },
          {
            description: 'Wavefront Object (*.obj)',
            accept: { 'text/plain': ['.obj'] }
          },
          {
            description: 'Standard Triangle Language (*.stl)',
            accept: { 'model/stl': ['.stl'] }
          },
          {
            description: 'Polygon File Format (*.ply)',
            accept: { 'application/x-ply': ['.ply'] }
          }
        ];

        // Sort so the matching extension is first in the list
        var matchingIdx = saveTypes.findIndex(function(t) {
          var exts = Object.values(t.accept)[0];
          return exts && exts.indexOf(ext) !== -1;
        });
        if (matchingIdx >= 0) {
          var matched = saveTypes.splice(matchingIdx, 1)[0];
          saveTypes.unshift(matched);
        } else {
          // If the extension wasn't found in saveTypes (e.g. unknown or generic), add it as the primary option
          var customType = {
            description: ext.toUpperCase() + ' File (*' + ext + ')',
            accept: {}
          };
          customType.accept[type || 'application/octet-stream'] = [ext];
          saveTypes.unshift(customType);
        }

        var handle = await window.showSaveFilePicker({
          suggestedName: name,
          types: saveTypes
        });
        var writable = await handle.createWritable();
        await writable.write(buffer_data);
        await writable.close();
        return;
      } catch (err) {
        // User cancelled picker or permission denied - fallback only if not AbortError
        if (err && err.name === 'AbortError') {
          return;
        }
        console.warn('showSaveFilePicker error, falling back to download:', err);
      }

      // Fallback: regular browser download
      var blob = new Blob([buffer_data], {type: type || 'application/octet-stream'});
      var url = URL.createObjectURL(blob);
      var a = document.createElement('a');
      a.href = url;
      a.download = name;
      a.rel = 'noopener';
      a.style.display = 'none';
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      setTimeout(function () { URL.revokeObjectURL(url); }, 0);
    })();
    return;
  }

  /// Fallback for browsers without File System Access API (Firefox, Safari)
  var blob = new Blob([buffer_data], {type: type || 'application/octet-stream'});
  var url = URL.createObjectURL(blob);
  setTimeout(function () {
    var a = document.createElement('a');
    a.href = url;
    a.download = name;
    a.rel = 'noopener';
    a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(function () { URL.revokeObjectURL(url); }, 0);
  }, 0);
});
#pragma GCC diagnostic pop

inline void download(std::string const &filename, std::string const &mime_type, std::string_view buffer, bool use_picker) {
  /// C++ wrapper for javascript download call, accepting a string_view
  download(filename.c_str(), mime_type.c_str(), (void const *)buffer.data(), buffer.size(), use_picker);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-variable-declarations"
EM_JS_INLINE(void, init_drag_and_drop, (), {
  var target = document.querySelector('canvas') || document.body;
  if (!target || target._vengi_dnd_initialized) {
    return;
  }
  target._vengi_dnd_initialized = true;

  target.addEventListener('dragover', function(e) {
    e.preventDefault();
    e.stopPropagation();
    if (e.dataTransfer) {
      e.dataTransfer.dropEffect = 'copy';
    }
  });

  target.addEventListener('dragenter', function(e) {
    e.preventDefault();
    e.stopPropagation();
  });

  target.addEventListener('drop', function(e) {
    e.preventDefault();
    e.stopPropagation();
    if (!e.dataTransfer || !e.dataTransfer.files || e.dataTransfer.files.length === 0) {
      return;
    }
    for (var i = 0; i < e.dataTransfer.files.length; ++i) {
      var file = e.dataTransfer.files[i];
      var reader = new FileReader();
      reader.onload = (function(f) {
        return function(event) {
          var uint8Arr = new Uint8Array(event.target.result);
          var data_ptr = Module["_malloc"](uint8Arr.length);
          var data_on_heap = new Uint8Array(Module["HEAPU8"].buffer, data_ptr, uint8Arr.length);
          data_on_heap.set(uint8Arr);
          Module["ccall"]('drop_file_return', 'number', ['string', 'number', 'number'], [f.name, data_on_heap.byteOffset, uint8Arr.length]);
          Module["_free"](data_ptr);
        };
      })(file);
      reader.readAsArrayBuffer(file);
    }
  });
});
#pragma GCC diagnostic pop

inline void initDragAndDrop() {
  init_drag_and_drop();
}

namespace {

extern "C" {

EMSCRIPTEN_KEEPALIVE inline int upload_file_return(char const *filename, char const *mime_type, char *buffer, size_t buffer_size, upload_handler callback, void *callback_data);
EMSCRIPTEN_KEEPALIVE inline int drop_file_return(char const *filename, char *buffer, size_t buffer_size);

EMSCRIPTEN_KEEPALIVE inline int drop_file_return(char const *filename, char *buffer, size_t buffer_size) {
  if (!filename || !buffer || buffer_size == 0) {
    return 0;
  }
  const core::String uploadedFilename = core::string::extractFilenameWithExtension(filename);
  if (uploadedFilename.empty()) {
    return 0;
  }
  io::MemoryReadStream stream(buffer, buffer_size);
  const io::FilesystemPtr &fs = io::filesystem();
  if (fs->homeWrite(uploadedFilename, stream) != (long)buffer_size) {
    return 0;
  }
  fs->sync();
  core::Singleton<video::EventHandler>::getInstance().dropFile(nullptr, uploadedFilename);
  return 1;
}

EMSCRIPTEN_KEEPALIVE inline int upload_file_return(char const *filename, char const *mime_type, char *buffer, size_t buffer_size, upload_handler callback, void *callback_data) {
  /// Load a file - this function is called from javascript when the file upload is activated

  /// The file was not uploaded.
  /// We must process this case separately because std::string_view(nullptr, 0) results in UB.
  /// <The behavior is undefined if [s, s + count) is not a valid range
  /// (even though the constructor may not access any of the elements of this range)>
  /// https://en.cppreference.com/w/cpp/string/basic_string_view/basic_string_view
  if(!buffer || buffer_size == 0) {
    callback(filename, mime_type, std::string_view(), callback_data);
    return 1;
  }
  /// Ok - note: the string_view is only valid for the duration of this call; do not store it for later use
  callback(filename, mime_type, {buffer, buffer_size}, callback_data);
  return 1;
}

}

}

}
