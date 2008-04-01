//
// The beng-proxy JavaScript library.
//
// Author: Max Kellermann <mk@cm4all.com>
//

function beng_widget_uri(base_uri, session_id, frame, focus, mode, path) {
    var uri = base_uri + ";session=" + escape(session_id);
    if (focus != null) {
        uri += "&focus=" + escape(focus);
        if (mode == "frame" || mode == "proxy" || mode == "save")
            frame = focus;
        if (frame != null)
            uri += "&frame=" + escape(frame);
        if (mode == "proxy")
            uri += "&raw=1";
        if (mode == "save")
            uri += "&save=1";
        if (path != null) {
            var query_string = null;
            var qmark = path.indexOf("?");
            if (qmark >= 0) {
                query_string = path.substring(qmark);
                path = path.substring(0, qmark);
            }
            uri += "&path=" + escape(path);
            if (query_string != null)
                uri += query_string;
        }
    }
    return uri;
}

function beng_proxy_request() {
    var req = null;
    if (window.XMLHttpRequest) {
    	try {
            return new XMLHttpRequest();
        } catch(e) {
            return null;
        }
    } else if(window.ActiveXObject) {
       	try {
            return new ActiveXObject("Msxml2.XMLHTTP");
      	} catch(e) {
            try {
                return new ActiveXObject("Microsoft.XMLHTTP");
            } catch(e) {
                return null;
            }
        }
    } else {
        return null;
    }
}

function beng_proxy_make_uri(focus, path, mode) {
    return beng_widget_uri(this.uri, this.session, null, focus, mode, path);
}

function beng_proxy(session) {
    this.uri = window.location.href.replace(/[;?#].*/, "");
    this.session = session;

    this.make_uri = beng_proxy_make_uri;

    return this;
}

function beng_widget_get_widget(id) {
    var slash = id.indexOf("/");
    if (slash == -1)
        return this.widgets[id];
    var widget = this.widgets[id.substring(0, slash)];
    if (widget == null)
        return null;
    return widget.getWidget(id.substring(slash + 1));
}

function beng_widget_create_widget(id) {
    return new beng_widget(this, id);
}

function beng_root_widget(proxy) {
    this.parent = null;
    this.proxy = proxy;
    this.id = null;
    this.path = null;
    this.widgets = Array();

    this.getWidget = beng_widget_get_widget;
    this.createWidget = beng_widget_create_widget;

    return this;
}

function beng_widget_translate_uri(uri, mode) {
    if (this.path == null)
        return null;

    return this.proxy.make_uri(this.path, uri, mode);
}

function beng_widget_get(uri, onreadystatechange, mode) {
    if (mode == null)
        mode = "proxy";
    var url = this.translateURI(uri, mode);
    if (url == null)
        return null;
    var req = beng_proxy_request();
    if (req == null)
        return null;
    req.onreadystatechange = onreadystatechange;
    req.open("GET", url, onreadystatechange != null);
    req.send(null);
    return req;
}

function _beng_widget_post(uri, content_type, request_body,
                           onreadystatechange) {
    if (content_type == null || request_body == null)
        return null;
    var url = this.translateURI(uri, "proxy");
    if (url == null)
        return null;
    var req = beng_proxy_request();
    if (req == null)
        return null;
    req.setRequestHeader("Content-Type", content_type);
    req.onreadystatechange = onreadystatechange;
    req.open("POST", url, onreadystatechange != null);
    req.send(request_body);
    return req;
}

function beng_widget_reload_inline(widget, uri) {
    var req = widget.get(uri, function() {
            if (req == null)
                return;
            if (req.readyState == 4 && req.status >= 200 && req.status < 300) {
                var element = widget.getElement();
                if (element != null)
                    element.innerHTML = req.responseText;
            }
        }, "frame");
    return req;
}

function beng_widget_reload_iframe(widget, iframe, uri) {
    var url = widget.translateURI(uri, true);
    if (url == null)
        return null;

    iframe.src = url;
    return null;
}

function beng_widget_reload(uri) {
    if (this.path == null)
        return null;

    var iframe_id = "beng_iframe___" + this.path.replace(/\//g, "__") + "__";
    var iframe = document.getElementById(iframe_id);
    if (iframe != null)
        return beng_widget_reload_iframe(this, iframe, uri);

    return beng_widget_reload_inline(this, uri);
}

function beng_widget(parent, id) {
    if (parent.parent == null)
        this.path = id;
    else if (parent.path == null)
        this.path = null;
    else
        this.path = parent.path + "/" + id;

    if (id != null)
        parent.widgets[id] = this;
    this.parent = parent;
    this.proxy = parent.proxy;
    this.id = id;
    this.widgets = Array();

    this.getWidget = beng_widget_get_widget;
    this.createWidget = beng_widget_create_widget;
    this.translateURI = beng_widget_translate_uri;
    this.get = beng_widget_get;
    this.post = _beng_widget_post;
    this.reload = beng_widget_reload;
    this.getElement = function() {
        if (this.path == null)
            return null;
        var id = "beng_widget___" + this.path.replace(/\//g, "__") + "__";
        return document.getElementById(id);
    };

    return this;
}

var _beng_onload_list = Array();

function _beng_onload() {
    var x;
    for (var x = 0; x < _beng_onload_list.length; ++x) {
        _beng_onload_list[x]();
    }
}

function beng_register_onload(onload) {
    _beng_onload_list.push(onload);
}
