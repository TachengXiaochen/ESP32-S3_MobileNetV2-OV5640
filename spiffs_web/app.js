// ESP32-S3 CAM AI Web 实时预览 - 前端逻辑

var pollTimer = null;
var streamLoaded = false;

// ---- 初始化 ----
document.addEventListener('DOMContentLoaded', function() {
    pollStatus();
    pollTimer = setInterval(pollStatus, 2000);
    loadAssetList();
});

// ---- 状态轮询 ----
function pollStatus() {
    fetch('/api/status')
        .then(function(r) { return r.json(); })
        .then(function(s) {
            setEl('st-heap',    fmtKB(s.free_heap),      s.free_heap > 100000 ? 'ok' : 'warn');
            setEl('st-minheap', fmtKB(s.min_free_heap),   s.min_free_heap > 100000 ? 'ok' : 'warn');
            setEl('st-camera',  s.camera_ready ? '✅ 就绪' : '❌ 未就绪', s.camera_ready ? 'ok' : 'err');
            setEl('st-storage', s.storage_ready ? '✅ 就绪 (SD卡)' : '❌ 未就绪', s.storage_ready ? 'ok' : 'err');
            setEl('st-bstate',  s.be_state, '');
            setEl('st-tagid',   s.current_tag_id || '(无)', '');
            setEl('st-wifi',    s.wifi_clients, '');
        })
        .catch(function() {
            setEl('st-heap', '--', 'err');
        });
}

// ---- 资产列表加载 ----
function loadAssetList() {
    fetch('/api/assets')
        .then(function(r) { return r.json(); })
        .then(function(data) {
            var sel = document.getElementById('tag-select');
            sel.innerHTML = '<option value="">-- 选择资产 --</option>';
            if (data.assets && data.assets.length > 0) {
                data.assets.forEach(function(a) {
                    var opt = document.createElement('option');
                    opt.value = a.tag_id;
                    opt.textContent = a.tag_id;
                    sel.appendChild(opt);
                });
            } else {
                sel.innerHTML = '<option value="">(无已注册资产)</option>';
            }
        })
        .catch(function() {
            document.getElementById('tag-select').innerHTML = '<option value="">(加载失败)</option>';
        });
}

// ---- FST 帧加载 ----
function loadFrames() {
    var tagId = document.getElementById('tag-select').value;
    if (!tagId) { alert('请先选择一个 Tag ID'); return; }

    // 重置显示
    ['front','side','top'].forEach(function(v) {
        document.getElementById('img-' + v).style.display = 'none';
        document.getElementById('img-' + v + '-na').style.display = 'inline';
        document.getElementById('info-' + v).textContent = '加载中...';
    });

    fetch('/api/frames?tag_id=' + encodeURIComponent(tagId))
        .then(function(r) { return r.json(); })
        .then(function(data) {
            data.frames.forEach(function(f) {
                var view = f.view;
                if (f.size > 0) {
                    document.getElementById('img-' + view).src = f.url + '&t=' + Date.now();
                    document.getElementById('img-' + view).style.display = 'block';
                    document.getElementById('img-' + view + '-na').style.display = 'none';
                    document.getElementById('info-' + view).textContent = fmtKB(f.size);
                } else {
                    document.getElementById('img-' + view).style.display = 'none';
                    document.getElementById('img-' + view + '-na').style.display = 'inline';
                    document.getElementById('info-' + view).textContent = '无图片';
                }
            });
        })
        .catch(function() {
            ['front','side','top'].forEach(function(v) {
                document.getElementById('info-' + v).textContent = '加载失败';
            });
        });
}

// ---- 快照 ----
function takeSnapshot() {
    window.open('/api/snapshot?t=' + Date.now(), '_blank');
}

// ---- 流状态 ----
function onStreamLoad() {
    streamLoaded = true;
    document.getElementById('stream-status').textContent = '● 实时';
    document.getElementById('stream-status').className = 'stream-status ok';
}
function onStreamError() {
    streamLoaded = false;
    document.getElementById('stream-status').textContent = '✗ 连接失败';
    document.getElementById('stream-status').className = 'stream-status err';
}

// ---- 辅助函数 ----
function setEl(id, text, cls) {
    var el = document.getElementById(id);
    if (el) {
        el.textContent = text;
        el.className = cls || '';
    }
}
function fmtKB(bytes) {
    if (bytes > 1048576) return (bytes/1048576).toFixed(1) + ' MB';
    if (bytes > 1024)    return (bytes/1024).toFixed(0) + ' KB';
    return bytes + ' B';
}
