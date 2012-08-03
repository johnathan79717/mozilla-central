/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

MARIONETTE_TIMEOUT = 10000;

const WHITELIST_PREF = "dom.telephony.app.phone.url";
SpecialPowers.setCharPref(WHITELIST_PREF, window.location.href);

let telephony = window.navigator.mozTelephony;
let number = "5555552368";
let outgoing;
let calls;

function verifyInitialState() {
  log("Verifying initial state.");
  ok(telephony);
  is(telephony.active, null);
  ok(telephony.calls);
  is(telephony.calls.length, 0);
  calls = telephony.calls;

  runEmulatorCmd("gsm list", function(result) {
    log("Initial call list: " + result);
    is(result[0], "OK");
    dial();
  });
}

function dial() {
  log("Make an outgoing call.");

  outgoing = telephony.dial(number);
  ok(outgoing);
  is(outgoing.number, number);
  is(outgoing.state, "dialing");

  is(outgoing, telephony.active);
  //ok(telephony.calls === calls); // bug 717414
  is(telephony.calls.length, 1);
  is(telephony.calls[0], outgoing);

  // Get call list. Answer a call if the connection is established.
  runEmulatorCmd("gsm list", cmdCallback);
}

function answer() {
  log("Answering the outgoing call.");

  // We get no "connecting" event when the remote party answers the call.

  outgoing.onconnected = function onconnected(event) {
    log("Received 'connected' call event.");
    is(outgoing, event.call);
    is(outgoing.state, "connected");

    is(outgoing, telephony.active);

    runEmulatorCmd("gsm list", function(result) {
      log("Call list is now: " + result);
      is(result[0], "outbound to  " + number + " : active");
      is(result[1], "OK");
      hold();
    });
  };
  runEmulatorCmd("gsm accept " + number);
};

function hold() {
  log("Holding the outgoing call.");

  outgoing.onholding = function onholding(event) {
    log("Received 'holding' call event.");
    is(outgoing, event.call);
    is(outgoing.state, "holding");

    is(outgoing, telephony.active);
  };

  outgoing.onheld = function onheld(event) {
    log("Received 'held' call event.");
    is(outgoing, event.call);
    is(outgoing.state, "held");

    is(telephony.active, null);
    is(telephony.calls.length, 1);

    runEmulatorCmd("gsm list", function(result) {
      log("Call list is now: " + result);
      is(result[0], "outbound to  " + number + " : held");
      is(result[1], "OK");
      hangUp();
    });
  };
  outgoing.hold();
}

function hangUp() {
  log("Hanging up the outgoing call.");

  let gotDisconnecting = false;
  outgoing.ondisconnecting = function ondisconnecting(event) {
    log("Received disconnecting call event.");
    is(outgoing, event.call);
    is(outgoing.state, "disconnecting");
    gotDisconnecting = true;
  };

  outgoing.ondisconnected = function ondisconnected(event) {
    log("Received 'disconnected' call event.");
    is(outgoing, event.call);
    is(outgoing.state, "disconnected");
    ok(gotDisconnecting);

    is(telephony.active, null);
    is(telephony.calls.length, 0);

    runEmulatorCmd("gsm list", function(result) {
      log("Call list is now: " + result);
      is(result[0], "OK");
      cleanUp();
    });
  };

  outgoing.hangUp();
}

function cmdCallback(result) {
  let unknownState = "outbound to  " + number + " : unknown";
  let alertingState = "outbound to " + number + " : alerting";

  log("Call list is now: " + result);

  switch (result[0]) {
    // Gsm list is empty. Wait until the connection is established.
    case "OK":
      runEmulatorCmd("gsm list", cmdCallback);
      break;
    // Answer the call now since the connection is established.
    case unknownState: // Fall through ...
    case alertingState:
      is(result[1], "OK");
      answer();
      break;
  }
}

function cleanUp() {
  SpecialPowers.clearUserPref(WHITELIST_PREF);
  finish();
}

verifyInitialState();
