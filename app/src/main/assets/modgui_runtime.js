(function() {
    var ports = $PORTS_JSON;
    var portMap = {};
    ports.forEach(function(p) { portMap[p.symbol] = p; });

    var KNOB_FRAMES = 65;
    var DRAG_SENSITIVITY = 110;
    var activeKnob = null;
    var startY = 0, startVal = 0, activeMeta = null, activeSymbol = null;

    // Prevent all default touch behavior on the entire page to avoid
    // scroll/zoom stealing events from knob drags
    document.addEventListener('touchmove', function(e) {
        if (activeKnob) e.preventDefault();
    }, {passive: false});

    document.querySelectorAll('[mod-role="input-control-port"]').forEach(function(el) {
        var symbol = el.getAttribute('mod-port-symbol');
        if (!symbol || !portMap[symbol]) return;
        var meta = portMap[symbol];
        var value = AndroidHost.getParameter(symbol);
        if (value === 0 && meta['default'] !== 0) value = meta['default'];

        el._modSymbol = symbol;
        el._modMeta = meta;
        el._modValue = value;

        // Detect toggle from CSS class (mod-on-off-image, mod-switch-image)
        // even if the LV2 port doesn't have lv2:toggled property.
        // Some plugins use lv2:integer + lv2:enumeration with 0/1 range instead.
        // Use a local flag — don't mutate meta.toggle since the bypass detector
        // uses it to find the actual bypass port.
        var isToggleElement = meta.toggle ||
                              el.classList.contains('mod-on-off-image') ||
                              el.classList.contains('mod-switch-image') ||
                              el.getAttribute('mod-widget') === 'switch';
        el._modIsToggle = isToggleElement;

        // Prevent browser from treating touches as scroll/pan
        el.style.touchAction = 'none';
        el.style.userSelect = 'none';
        el.style.webkitUserSelect = 'none';
        el.style.cursor = 'pointer';

        var parentKnob = el.closest('.mod-knob, .mod-knob-trim');
        if (parentKnob) {
            parentKnob.style.touchAction = 'none';
            parentKnob.style.cursor = 'pointer';
        }

        if (isToggleElement) {
            // Toggle port: click to switch on/off
            updateToggleVisual(el);
            function onToggle(e) {
                e.preventDefault();
                e.stopPropagation();
                var newVal = el._modValue > 0.5 ? meta.min : meta.max;
                el._modValue = newVal;
                updateToggleVisual(el);
                AndroidHost.setParameter(symbol, newVal);
            }
            el.addEventListener('click', onToggle);
            if (parentKnob && parentKnob !== el) {
                parentKnob.addEventListener('click', onToggle);
            }
        } else {
            // Continuous port: knob drag
            updateKnobVisual(el);

            function onStart(e) {
                e.preventDefault();
                e.stopPropagation();
                var pt = e.touches ? e.touches[0] : e;
                activeKnob = el;
                activeMeta = meta;
                activeSymbol = symbol;
                startY = pt.clientY;
                startVal = el._modValue;
                if (typeof AndroidHost.setKnobActive === 'function') AndroidHost.setKnobActive(true);
            }

            el.addEventListener('touchstart', onStart, {passive: false});
            el.addEventListener('mousedown', onStart);

            if (parentKnob && parentKnob !== el) {
                parentKnob.addEventListener('touchstart', onStart, {passive: false});
                parentKnob.addEventListener('mousedown', onStart);
            }
        }
    });

    // Global move/end handlers (always attached, check activeKnob)
    function onMove(e) {
        if (!activeKnob) return;
        e.preventDefault();
        var pt = e.touches ? e.touches[0] : e;
        var dy = startY - pt.clientY;
        var range = activeMeta.max - activeMeta.min;
        var newVal = startVal + (dy / DRAG_SENSITIVITY) * range;
        newVal = Math.max(activeMeta.min, Math.min(activeMeta.max, newVal));
        activeKnob._modValue = newVal;
        updateKnobVisual(activeKnob);
        AndroidHost.setParameter(activeSymbol, newVal);
    }

    function onEnd(e) {
        if (activeKnob && typeof AndroidHost.setKnobActive === 'function') AndroidHost.setKnobActive(false);
        activeKnob = null;
        activeMeta = null;
        activeSymbol = null;
    }

    document.addEventListener('touchmove', onMove, {passive: false});
    document.addEventListener('touchend', onEnd);
    document.addEventListener('touchcancel', onEnd);
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onEnd);

    // Called from Android to push parameter updates from other UIs (X11, sliders)
    window._modRefreshPorts = function() {
        document.querySelectorAll('[mod-role="input-control-port"]').forEach(function(el) {
            if (!el._modSymbol || !el._modMeta) return;
            var newVal = AndroidHost.getParameter(el._modSymbol);
            if (Math.abs(newVal - el._modValue) > 0.0001) {
                el._modValue = newVal;
                if (el._modIsToggle) {
                    updateToggleVisual(el);
                } else {
                    updateKnobVisual(el);
                }
            }
        });
        // Also refresh bypass
        if (typeof window._modRefreshBypass === 'function') window._modRefreshBypass();
    };

    // Bypass footswitch — find the toggle port (lv2:enabled / lv2:toggled)
    // and wire the footswitch to it
    var bypassPort = null;
    ports.forEach(function(p) {
        if (p.toggle) bypassPort = p;
    });
    document.querySelectorAll('[mod-role="bypass"]').forEach(function(el) {
        var enabled = bypassPort ? AndroidHost.getParameter(bypassPort.symbol) > 0.5 : true;
        el.style.cursor = 'pointer';
        el.style.touchAction = 'none';
        var light = document.querySelector('[mod-role="bypass-light"]');
        function updateBypassVisual() {
            el.classList.toggle('on', enabled);
            el.classList.toggle('off', !enabled);
            if (light) {
                light.classList.toggle('on', enabled);
                light.classList.toggle('off', !enabled);
            }
        }
        updateBypassVisual();
        el.addEventListener('click', function() {
            enabled = !enabled;
            updateBypassVisual();
            if (bypassPort) {
                AndroidHost.setParameter(bypassPort.symbol, enabled ? bypassPort.max : bypassPort.min);
            }
        });
        window._modRefreshBypass = function() {
            if (bypassPort) {
                var newEnabled = AndroidHost.getParameter(bypassPort.symbol) > 0.5;
                if (newEnabled !== enabled) {
                    enabled = newEnabled;
                    updateBypassVisual();
                }
            }
        };
    });

    function updateToggleVisual(el) {
        var isOn = el._modValue > 0.5;
        // For mod-on-off-image sprite: on = second frame, off = first frame
        // Sprites are horizontal strips; each frame is one element-width wide.
        // Preserve original vertical position (typically 0px) from the stylesheet.
        if (el.classList.contains('mod-on-off-image')) {
            var cs = window.getComputedStyle(el);
            var w = el.offsetWidth || parseInt(cs.width) || 60;
            // Parse current Y position to preserve it (default to 0px)
            var bgPos = cs.backgroundPosition || '0px 0px';
            var yPos = bgPos.split(/\s+/)[1] || '0px';
            el.style.backgroundPosition = (isOn ? -w : 0) + 'px ' + yPos;
        }
        // For elements using on/off CSS classes (mod-switch-image, flipsw, etc.)
        el.classList.toggle('on', isOn);
        el.classList.toggle('off', !isOn);
    }

    function updateKnobVisual(el) {
        var meta = el._modMeta;
        var norm = (el._modValue - meta.min) / (meta.max - meta.min);
        norm = Math.max(0, Math.min(1, norm));

        // Rotation-based knobs (e.g. AIDA-X): mod-widget-rotation="270"
        var rotRange = el.getAttribute('mod-widget-rotation');
        if (rotRange) {
            var range = parseFloat(rotRange) || 270;
            var angle = -(range / 2) + norm * range;
            el.style.transform = 'rotate(' + angle + 'deg)';
            return;
        }

        var cs = window.getComputedStyle(el);
        var w = el.offsetWidth || parseInt(cs.width) || 64;

        // Detect actual frame count from sprite background-size rather than using
        // the hardcoded default — sprites vary per plugin (e.g. NAM has 50 frames).
        // background-size may be "NNNpx HHHpx" (explicit width) or "auto HHHpx"
        // (auto width, explicit height). parseInt("auto ...") → NaN, causing the
        // fallback 65-frame assumption — which pushes switch sprites (2-5 frames)
        // far off-screen and makes them invisible. Resolve "auto" width from the
        // image's natural dimensions instead (already cached by the browser).
        var bgSize = cs.backgroundSize || '';
        var bgParts = bgSize.trim().split(/\s+/);
        var bgW = parseInt(bgParts[0]);
        if (isNaN(bgW)) {
            // Cache resolved sprite width on the element to avoid repeated work.
            if (!el._modSpriteW) {
                var urlMatch = (cs.backgroundImage || '').match(/url\(['"]?([^'")\s]+)['"]?\)/);
                if (urlMatch) {
                    var spriteImg = new Image();
                    spriteImg.onload = function() {
                        // Deferred: image was not yet loaded at init time.
                        // Re-run once loaded so the initial frame renders correctly.
                        var bph = parseInt(bgParts[1]) || el.offsetHeight || w;
                        el._modSpriteW = spriteImg.naturalWidth * (bph / spriteImg.naturalHeight);
                        updateKnobVisual(el);
                    };
                    spriteImg.src = urlMatch[1];
                    if (spriteImg.complete && spriteImg.naturalWidth > 0) {
                        // Already cached — resolve synchronously and cancel deferred handler.
                        spriteImg.onload = null;
                        var targetH = parseInt(bgParts[1]) || el.offsetHeight || w;
                        el._modSpriteW = spriteImg.naturalWidth * (targetH / spriteImg.naturalHeight);
                    }
                }
            }
            bgW = el._modSpriteW || (KNOB_FRAMES * w);
        }
        var frames = Math.max(1, Math.round(bgW / w));

        var frame = Math.round(norm * (frames - 1));
        el.style.backgroundPosition = (-frame * w) + 'px center';
    }

    // Path parameter (model selector) support — e.g. NAM model selector
    // Toggle the model list when clicking the selected-value area
    document.querySelectorAll('[mod-widget="custom-select-path"]').forEach(function(el) {
        var selected = el.querySelector('.mod-enumerated-selected');
        var list = el.querySelector('.mod-enumerated-list');
        if (selected && list) {
            selected.style.cursor = 'pointer';
            selected.addEventListener('click', function(e) {
                e.preventDefault();
                e.stopPropagation();
                // If no models loaded (list only has the "..." browse entry or is empty),
                // open file picker directly instead of showing the dropdown
                var modelCount = list.querySelectorAll('[mod-role="enumeration-option"]').length;
                if (modelCount === 0) {
                    AndroidHost.requestFilePicker();
                } else {
                    list.style.display = list.style.display === 'block' ? 'none' : 'block';
                }
            });
        }
    });
    // Close list when clicking outside
    document.addEventListener('click', function() {
        document.querySelectorAll('.mod-enumerated-list').forEach(function(el) {
            el.style.display = 'none';
        });
    });
    // Allow scrolling inside the model list by preventing parent from stealing touches
    document.querySelectorAll('.mod-enumerated-list').forEach(function(el) {
        el.addEventListener('touchstart', function(e) {
            e.stopPropagation();
            if (typeof AndroidHost.setKnobActive === 'function') AndroidHost.setKnobActive(true);
        }, {passive: true});
        el.addEventListener('touchend', function() {
            if (typeof AndroidHost.setKnobActive === 'function') AndroidHost.setKnobActive(false);
        });
        el.addEventListener('touchcancel', function() {
            if (typeof AndroidHost.setKnobActive === 'function') AndroidHost.setKnobActive(false);
        });
    });

    // Called from Android to populate the model list
    // modelsJson: '[{"name":"Model A","path":"/data/.../model_a.nam"}, ...]'
    window._modSetModelList = function(modelsJson) {
        var models = JSON.parse(modelsJson);
        document.querySelectorAll('.mod-enumerated-list').forEach(function(listEl) {
            listEl.innerHTML = '';
            // "..." browse entry
            var browseDiv = document.createElement('div');
            browseDiv.textContent = '\u2026';
            browseDiv.style.fontWeight = 'bold';
            browseDiv.addEventListener('click', function(e) {
                e.stopPropagation();
                listEl.style.display = 'none';
                AndroidHost.requestFilePicker();
            });
            listEl.appendChild(browseDiv);
            // Model entries
            models.forEach(function(m) {
                var div = document.createElement('div');
                div.textContent = m.name;
                div.setAttribute('mod-role', 'enumeration-option');
                div.setAttribute('mod-parameter-value', m.path);
                div.addEventListener('click', function(e) {
                    e.stopPropagation();
                    listEl.style.display = 'none';
                    // Update selected display
                    var parent = listEl.closest('[mod-widget="custom-select-path"]');
                    if (parent) {
                        var sel = parent.querySelector('.mod-enumerated-selected');
                        if (sel) sel.textContent = m.name;
                    }
                    // Mark selected
                    listEl.querySelectorAll('div').forEach(function(d) { d.classList.remove('selected'); });
                    div.classList.add('selected');
                    AndroidHost.selectModelPath(m.path);
                });
                listEl.appendChild(div);
            });
        });
    };

    // Called from Android to update the display text for the active model
    window._modSetPathDisplay = function(displayName) {
        document.querySelectorAll('[mod-role="input-parameter-value"]').forEach(function(el) {
            el.textContent = displayName;
        });
    };
})();
