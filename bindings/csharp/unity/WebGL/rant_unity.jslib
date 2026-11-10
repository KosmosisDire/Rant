// A WebGL player's way to the mesh: the tunnel to a Rant bridge, whose sockets Rant compiled into
// the player uses. RantTunnel comes from rant_tunnel.jspre, Rant's own TunnelNet.

mergeInto(LibraryManager.library, {
    // Opens the tunnel and makes it the net object nodes open on, then calls done(id, error),
    // error 0 once connected, else the reason as a C string valid during the call. A later
    // connect or a disconnect supersedes this one.
    RantWebGLConnect: function (url, passphrase, name, id, done) {
        var options = { passphrase: UTF8ToString(passphrase), name: UTF8ToString(name) || "unity" };
        var fail = function (text) {
            var why = stringToNewUTF8(text);
            {{{ makeDynCall('vii', 'done') }}}(id, why);
            _free(why);
        };
        Module["rantNetFor"] = id;
        RantTunnel.TunnelNet.connect(UTF8ToString(url), options).then(function (net) {
            if (Module["rantNetFor"] !== id) {
                net.shutdown();
                fail("superseded by a newer connect");
                return;
            }
            if (Module["rantNet"]) Module["rantNet"].shutdown();
            Module["rantNet"] = net;
            {{{ makeDynCall('vii', 'done') }}}(id, 0);
        }, function (e) {
            fail(String((e && e.message) || e));
        });
    },
    RantWebGLConnect__deps: ['$stringToNewUTF8', 'free'],

    // Ends the tunnel, once its nodes closed, and any connect still on its way.
    RantWebGLDisconnect: function () {
        Module["rantNetFor"] = 0;
        if (Module["rantNet"]) Module["rantNet"].shutdown();
        Module["rantNet"] = null;
    },
});
