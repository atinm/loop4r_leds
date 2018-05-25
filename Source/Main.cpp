/*
 * This file is part of loop4r_control.
 * Copyright (C) 2018 Atin Malaviya.  https://www.github.com/atinm
 *
 * loop4r_control is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * loop4r_control is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 ==============================================================================
 loop4r_control for controlling sooperlooper via an FCB1010 with the EurekaProm
 set to I/O mode. The goal is to allow control of sooperlooper via just the
 controller and have the LEDs etc reflect the state of sooperlooper.

 Forked from the https://github.com/gbevin/ReceiveMIDI and
 https://github.com/gbevin/SendMIDI source as a MIDI starting point.
 ==============================================================================
 */

#include "../JuceLibraryCode/JuceHeader.h"
#include <alsa/asoundlib.h>
#include <sstream>


//==============================================================================
enum CommandIndex
{
    NONE,
    LIST,
    DEVICE_OUT,
    CHANNEL,
    OSC_IN,
    OSC_OUT
};

enum LedStates
{
    Dark,
    Light,
    Blink,
    FastBlink
};

static const int NUM_LED_PEDALS = 10;
static const int UP = 10;
static const int DOWN = 11;

// timers
static const int TIMER_OFF = 0;
static const int TIMER_FASTBLINK = 1;
static const int TIMER_BLINK = 3;

struct ApplicationCommand
{
    static ApplicationCommand Dummy()
    {
	return {"", "", NONE, 0, "", ""};
    }

    void clear()
    {
	param_ = "";
	command_ = NONE;
	expectedOptions_ = 0;
	optionsDescription_ = "";
	commandDescription_ = "";
	opts_.clear();
    }

    String param_;
    String altParam_;
    CommandIndex command_;
    int expectedOptions_;
    String optionsDescription_;
    String commandDescription_;
    StringArray opts_;
};

struct LED {
    int index_;
    bool on_;
    int timer_;
    LedStates state_;

    void clear()
    {
	on_ = false;
	timer_ = TIMER_OFF;
	state_ = Dark;
    }
};

inline float sign(float value)
{
    return (float)(value > 0.) - (value < 0.);
}

class loop4r_ledsApplication  : public JUCEApplicationBase,
public Timer, private OSCReceiver::Listener<OSCReceiver::MessageLoopCallback>
{
public:
    //==============================================================================
    loop4r_ledsApplication()
    {
	commands_.add({"dout",  "device out",       DEVICE_OUT,         1, "name",           "Set the name of the MIDI output port"});
	commands_.add({"list",  "",                 LIST,               0, "",               "Lists the MIDI ports"});
	commands_.add({"ch",    "channel",          CHANNEL,            1, "number",         "Set MIDI channel for the commands (0-16), defaults to 0"});
	commands_.add({"oin",   "osc in",           OSC_IN,             1, "number",         "OSC receive port"});
	commands_.add({"oout",  "osc out",          OSC_OUT,            1, "number",         "OSC send port"});

	for (auto i=0; i<10; i++)
	{
	    leds_.add({i, false, TIMER_OFF, Dark});
	}

	channel_ = 1;
	useHexadecimalsByDefault_ = false;
	oscSendPort_ = 9000;
	oscReceivePort_ = 9001;
	currentCommand_ = ApplicationCommand::Dummy();
    }

    const String getApplicationName() override       { return ProjectInfo::projectName; }
    const String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override       { return false; }

    //==============================================================================
    void initialise (const String& commandLine) override
    {
	StringArray cmdLineParams(getCommandLineParameterArray());
	if (cmdLineParams.contains("--help") || cmdLineParams.contains("-h"))
	{
	    printUsage();
	    systemRequestedQuit();
	    return;
	}
	else if (cmdLineParams.contains("--version"))
	{
	    printVersion();
	    systemRequestedQuit();
	    return;
	}

	parseParameters(cmdLineParams);

	if (cmdLineParams.contains("--"))
	{
	    while (std::cin)
	    {
		std::string line;
		getline(std::cin, line);
		StringArray params = parseLineAsParameters(line);
		parseParameters(params);
	    }
	}

	if (cmdLineParams.isEmpty())
	{
	    printUsage();
	    systemRequestedQuit();
	}
	else
	{
	    startTimer(200);
	}
    }

    void timerCallback() override
    {
	int err;
	if (midiOutName_.isNotEmpty() && midiOut_ == nullptr)
	{
	    err = snd_rawmidi_open(NULL,&midiOut_, midiOutName_.toRawUTF8(), 0);
	    if (err)
	    {
		std::cerr << "Couldn't open MIDI output port \"" << midiOutName_ << "\"" << std::endl;
	    }
	    else
	    {
		// initialize the pedal leds to off
		for (auto i=0; i<NUM_LED_PEDALS; i++)
		    ledOff(i);
	    }
	}


	if (currentReceivePort_ < 0 || currentSendPort_ < 0) {
	    if (tryToConnectOsc())
	    {
		std::cerr << "Connected to OSC ports " << (int)currentReceivePort_ << " (in) and " << (int) currentSendPort_ << " (out)" << std::endl;
		heartbeat_ = 5;
	    }
	}
	else
	{
	    // heartbeat
	    if (heartbeat_ == 0)
	    {
		oscSender.send("/loop4r/ping", (String) "127.0.0.1", (int) currentReceivePort_, (String) "/heartbeat");
	    }
	    else if (heartbeat_ < -5) // give a second before we try reconnecting
	    {
		// we've lost heartbeat, try reconnecting
		currentReceivePort_ = -1;
		currentSendPort_ = -1;
		if (tryToConnectOsc())
		{
		    std::cerr << "Reconnected to OSC ports " << (int)currentReceivePort_ << " (in) and " << (int) currentSendPort_ << " (out)" << std::endl;
		    heartbeat_ = 5;
		}
	    }
	    else
	    {
		--heartbeat_;
	    }

	    // handle pedal led state for blinking pedals
	    for (auto&& led : leds_)
	    {
		if (led.state_ == Blink || led.state_ == FastBlink)
		{
		    if (led.timer_ <= 0)
		    {
			if (led.on_) {
			    ledOff(led.index_);
			}
			else
			{
			    ledOn(led.index_);
			}
			led.timer_ = led.state_ == Blink ? TIMER_BLINK : TIMER_FASTBLINK;
		    }
		    else
		    {
			led.timer_--;
		    }
		}
	    }
	}
    }

    void shutdown() override
    {
	// Add your application's shutdown code here..


    }

    //==============================================================================
    void systemRequestedQuit() override
    {
	// This is called when the app is being asked to quit: you can ignore this
	// request and let the app carry on running, or call quit() to allow the app to close.
	quit();
    }

    void anotherInstanceStarted (const String& commandLine) override
    {
	// When another instance of the app is launched while this one is running,
	// this method is invoked, and the commandLine parameter tells you what
	// the other instance's command-line arguments were.
    }
    void suspended() override {}
    void resumed() override {}
    void unhandledException(const std::exception*, const String&, int) override { jassertfalse; }

private:
    ApplicationCommand* findApplicationCommand(const String& param)
    {
	for (auto&& cmd : commands_)
	{
	    if (cmd.param_.equalsIgnoreCase(param) || cmd.altParam_.equalsIgnoreCase(param))
	    {
		return &cmd;
	    }
	}
	return nullptr;
    }

    StringArray parseLineAsParameters(const String& line)
    {
	StringArray parameters;
	if (!line.startsWith("#"))
	{
	    StringArray tokens;
	    tokens.addTokens(line, true);
	    tokens.removeEmptyStrings(true);
	    for (String token : tokens)
	    {
		parameters.add(token.trimCharactersAtStart("\"").trimCharactersAtEnd("\""));
	    }
	}
	return parameters;
    }

    void handleVarArgCommand()
    {
	if (currentCommand_.expectedOptions_ < 0)
	{
	    executeCommand(currentCommand_);
	}
    }

    void parseParameters(StringArray& parameters)
    {
	for (String param : parameters)
	{
	    if (param == "--") continue;

	    ApplicationCommand* cmd = findApplicationCommand(param);
	    if (cmd)
	    {
		handleVarArgCommand();

		currentCommand_ = *cmd;
	    }
	    else if (currentCommand_.command_ == NONE)
	    {
		File file = File::getCurrentWorkingDirectory().getChildFile(param);
		if (file.existsAsFile())
		{
		    parseFile(file);
		}
	    }
	    else if (currentCommand_.expectedOptions_ != 0)
	    {
		currentCommand_.opts_.add(param);
		currentCommand_.expectedOptions_ -= 1;
	    }

	    // handle fixed arg commands
	    if (currentCommand_.expectedOptions_ == 0)
	    {
		executeCommand(currentCommand_);
	    }
	}

	handleVarArgCommand();
    }

    void parseFile(File file)
    {
	StringArray parameters;

	StringArray lines;
	file.readLines(lines);
	for (String line : lines)
	{
	    parameters.addArray(parseLineAsParameters(line));
	}

	parseParameters(parameters);
    }

    bool checkChannel(const MidiMessage& msg, int channel)
    {
	return channel == 0 || msg.getChannel() == channel;
    }

    String output7BitAsHex(int v)
    {
	return String::toHexString(v).paddedLeft('0', 2).toUpperCase();
    }

    String output7Bit(int v)
    {
	if (useHexadecimalsByDefault_)
	{
	    return output7BitAsHex(v);
	}
	else
	{
	    return String(v);
	}
    }

    String output14BitAsHex(int v)
    {
	return String::toHexString(v).paddedLeft('0', 4).toUpperCase();
    }

    String output14Bit(int v)
    {
	if (useHexadecimalsByDefault_)
	{
	    return output14BitAsHex(v);
	}
	else
	{
	    return String(v);
	}
    }

    String outputChannel(const MidiMessage& msg)
    {
	return output7Bit(msg.getChannel()).paddedLeft(' ', 2);
    }

    bool tryToConnectOsc() {
	if (currentSendPort_ < 0) {
	    if (oscSender.connect ("127.0.0.1", oscSendPort_)) {
		std::cout << "Successfully connected to OSC Send port " << (int)oscSendPort_ << std::endl;
		currentSendPort_ = oscSendPort_;
	    }
	}

	if (currentReceivePort_ < 0) {
	    connect();
	}

	if (currentSendPort_ > 0 && currentReceivePort_ > 0) {
	    if (!pinged_)
	    {
		oscSender.send("/loop4r/ping", (String) "127.0.0.1", (int) currentReceivePort_, (String) "/pingack");
	    }
	    return true;
	}

	return false;
    }

    void executeCommand(ApplicationCommand& cmd)
    {
	switch (cmd.command_)
	{
	    case NONE:
		break;
	    case LIST:
		std::cout << "MIDI Input devices:" << std::endl;
		for (auto&& device : MidiInput::getDevices())
		{
		    std::cout << device << std::endl;
		}
		std::cout << "MIDI Output devices:" << std::endl;
		for (auto&& device : MidiOutput::getDevices())
		{
		    std::cout << device << std::endl;
		}
		systemRequestedQuit();
		break;
	    case CHANNEL:
		channel_ = asDecOrHex7BitValue(cmd.opts_[0]);
		break;
	    case DEVICE_OUT:
	    {
		int err;
		midiOut_ = nullptr;
		midiOutName_ = cmd.opts_[0];
		err = snd_rawmidi_open(NULL,&midiOut_, midiOutName_.toRawUTF8(), 0);
		if (err)
		{
		    std::cerr << "Couldn't open MIDI output port \"" << midiOutName_ << "\"" << std::endl;
		}
		else
		{
		    // initialize the pedal leds to off
		    for (auto i=0; i<NUM_LED_PEDALS; i++)
			ledOff(i);
		}
		break;
	    }
	    case OSC_OUT:
		oscSendPort_ = asPortNumber(cmd.opts_[0]);
		// specify here where to send OSC messages to: host URL and UDP port number
		if (! oscSender.connect ("127.0.0.1", oscSendPort_))
		    std::cerr << "Error: could not connect to UDP port " << cmd.opts_[0] << std::endl;
		else
		    currentSendPort_ = oscSendPort_;
		break;
	    case OSC_IN:
		oscReceivePort_ = asPortNumber(cmd.opts_[0]);
		if (!tryToConnectOsc())
		    std::cerr << "Error: could not connect to UDP port " << cmd.opts_[0] << std::endl;
		break;
	    default:
		filterCommands_.add(cmd);
		break;
	}
    }

    uint16 asPortNumber(String value)
    {
	return (uint16)limit16Bit(asDecOrHexIntValue(value));
    }

    uint8 asDecOrHex7BitValue(String value)
    {
	return (uint8)limit7Bit(asDecOrHexIntValue(value));
    }

    uint16 asDecOrHex14BitValue(String value)
    {
	return (uint16)limit14Bit(asDecOrHexIntValue(value));
    }

    int asDecOrHexIntValue(String value)
    {
	if (value.endsWithIgnoreCase("H"))
	{
	    return value.dropLastCharacters(1).getHexValue32();
	}
	else if (value.endsWithIgnoreCase("M"))
	{
	    return value.getIntValue();
	}
	else if (useHexadecimalsByDefault_)
	{
	    return value.getHexValue32();
	}
	else
	{
	    return value.getIntValue();
	}
    }

    static uint8 limit7Bit(int value)
    {
	return (uint8)jlimit(0, 0x7f, value);
    }

    static uint16 limit14Bit(int value)
    {
	return (uint16)jlimit(0, 0x3fff, value);
    }

    static uint16 limit16Bit(int value)
    {
	return (uint16)jlimit(0, 0xffff, value);
    }

    int pedalIndex(int controllerValue) {
	switch (controllerValue) {
	    case 1:
	    case 2:
	    case 3:
	    case 4:
	    case 5:
	    case 6:
	    case 7:
	    case 8:
	    case 9:
		return controllerValue-1;
	    case 0:
		return 9;
	    case 10:
		return UP;
	    case 11:
		return DOWN;
	    default:
		return controllerValue;
	}
    }

    uint8 ledNumber(int pedalIdx) {
	switch (pedalIdx) {
	    case 0:
	    case 1:
	    case 2:
	    case 3:
	    case 4:
	    case 5:
	    case 6:
	    case 7:
	    case 8:
		return pedalIdx+1;
	    case 9:
		return 0;
	    default:
		return pedalIdx;
	}
    }

    void ledOn(int pedalIdx) {
	int wrote;
	leds_.getReference(pedalIdx).on_ = true;
	//sendMidiMessage(midiOut_, MidiMessage::controllerEvent(channel_, 106, ledNumber(pedalIdx)));
	unsigned char ch[]={MIDI_CMD_CONTROL, 106, ledNumber(pedalIdx)};
	wrote = snd_rawmidi_write(midiOut_, &ch, sizeof(ch));
	if (wrote != sizeof(ch))
	{
	    std::cerr << "Could not write CC " << (int)106 << " " << (int)ledNumber(pedalIdx) << std::endl;
	}
    }

    void ledOff(int pedalIdx) {
	int wrote;
	leds_.getReference(pedalIdx).on_ = false;
	//sendMidiMessage(midiOut_, MidiMessage::controllerEvent(channel_, 107, ledNumber(pedalIdx)));
	unsigned char ch[]={MIDI_CMD_CONTROL, 107, ledNumber(pedalIdx)};
	wrote = snd_rawmidi_write(midiOut_, &ch, sizeof(ch));
	if (wrote != sizeof(ch))
	{
	    std::cerr << "Could not write CC " << (int)107 << " " << (int)ledNumber(pedalIdx) << std::endl;
	}
    }

    void updateLeds()
    {
	for (auto&& led : leds_)
	{
	    updateLedState(led);
	}
    }

    void updateLedState(LED& led)
    {
	int wrote;
	//sendMidiMessage(midiOut_, MidiMessage::controllerEvent(channel_, led.on_? 106 : 107, ledNumber(led.index_)));
	unsigned char ch[]={MIDI_CMD_CONTROL,(unsigned char)(led.on_?106:107),ledNumber(led.index_)};
	wrote = snd_rawmidi_write(midiOut_, &ch, sizeof(ch));
	if (wrote != sizeof(ch))
	{
	    std::cerr << "Could not write CC " << (int)(led.on_?106:107) << " " << (int)ledNumber(led.index_) << std::endl;
	}
    }

    void getCurrentState(int index)
    {
	String buf = "/loop4r";
	oscSender.send(buf + "/leds",
		       (String) "127.0.0.1",
		       (int) currentReceivePort_,
		       (String) "/led");

	oscSender.send(buf + "/display",
		       (String) "127.0.0.1",
		       (int) currentReceivePort_,
		       (String) "/display");
    }

    void registerAutoUpdates(bool unreg)
    {
	String buf = "/loop4r";
	if (unreg)
	{
	    buf = buf + "/unregister_auto_update";
	    oscSender.send(buf,
			   (String) "127.0.0.1",
			   (int) currentReceivePort_);
	}
	else{
	    buf = buf + "/register_auto_update";
	    oscSender.send(buf,
			   (String) "127.0.0.1",
			   (int) currentReceivePort_);
	}
    }

    void handlePingAckMessage(const OSCMessage& message)
    {
	if (! message.isEmpty())
	{
	    int i = 0;
	    for (OSCArgument* arg = message.begin(); arg != message.end(); ++arg)
	    {
		switch (i)
		{
		    case 0:
			if (arg->isString())
			    hostUrl_ = arg->getString();
			break;
		    case 1:
			if (arg->isString())
			    version_ = arg->getString();
			break;
		    case 2:
			if (arg->isInt32())
			    ledCount_ = arg->getInt32();
			break;
		    case 3:
			if (arg->isInt32())
			    engineId_ = arg->getInt32();
			break;
		    default:
			std::cerr << "Unexpected number of arguments for /pingack" << std::endl;
			return;
		}
		i++;
	    }
	    if (ledCount_ > 0)
	    {
		leds_.clear();
		for (i = 0; i < ledCount_; i++)
		{
		    leds_.add({i, false, TIMER_OFF, Dark});
		    registerAutoUpdates(false);
		    getCurrentState(i);
		    updateLeds();
		}
	    }
	    heartbeat_ = 5; // we just heard from the looper
	}
    }

    void handleHeartbeatMessage(const OSCMessage& message)
    {
	if (! message.isEmpty())
	{
	    int i = 0;
	    int numleds = 0;
	    int uid = engineId_;
	    for (OSCArgument* arg = message.begin(); arg != message.end(); ++arg)
	    {
		switch (i)
		{
		    case 0:
			if (arg->isString())
			    hostUrl_ = arg->getString();
			break;
		    case 1:
			if (arg->isString())
			    version_ = arg->getString();
			break;
		    case 2:
			if (arg->isInt32())
			    numleds = arg->getInt32();
			break;
		    case 3:
			if (arg->isInt32())
			    uid = arg->getInt32();
			break;
		    default:
			std::cerr << "Unexpected number of arguments for /heartbeat" << std::endl;
		}
		i++;
	    }

	    if (uid != engineId_) {
		// looper changed on us, reinitialize
		if (numleds > 0)
		{
		    ledCount_ = numleds;
		    leds_.clear();
		    for (i = 0; i < ledCount_; i++)
		    {
			leds_.add({i, false, TIMER_OFF, Dark});
			registerAutoUpdates(false);
			getCurrentState(i);
			updateLeds();
		    }
		}
	    }
	    else
	    {
		// check loopcount
		if (ledCount_ != numleds)
		{
		    for (auto i=ledCount_; i<numleds; i++)
		    {
			registerAutoUpdates(false);
			leds_.add({i, false, TIMER_OFF, Dark});
			updateLeds();
		    }
		    ledCount_ = numleds;
		}
	    }

	    unsigned char ch[]={MIDI_CMD_CONTROL, (unsigned char)(heartbeatOn_ ? 107 : 106), (unsigned char)23};
	    ssize_t wrote = snd_rawmidi_write(midiOut_, &ch, sizeof(ch));
	    if (wrote != sizeof(ch))
	    {
	      std::cerr << "Could not write CC " << (int)(heartbeatOn_ ? 107 :106) << " " << 23 << std::endl;
	    }
	    heartbeatOn_ = !heartbeatOn_;
	    heartbeat_ = 5; // we just heard from the looper
	}
    }

    void handleLedMessage(const OSCMessage& message)
    {
	if (! message.isEmpty())
	{
	    OSCArgument* arg = message.begin();
	    int ledIndex = -1;
	    if (arg->isInt32())
	    {
		ledIndex = arg->getInt32();
		++arg;
	    }
	    else {
		std::cerr << "unrecognized format for led message." << std::endl;
		return;
	    }

	    if (ledIndex < 0 || ledIndex >= ledCount_) {
		return;
	    }
	    else
	    {
		LED& led = leds_.getReference(ledIndex);
		if (arg->isInt32())
		{
		    led.on_ = (arg->getInt32() == 0 ? false : true);
		    ++arg;

		    if (arg->isInt32())
		    {
			led.timer_ = arg->getInt32();
			++arg;

			if (arg->isInt32())
			{
			    led.state_ = static_cast<LedStates>(arg->getInt32());

			    updateLedState(led);
			}
			else {
			    std::cerr << "unrecognized format for led message." << std::endl;
			    return;
			}
		    }
		    else {
			std::cerr << "unrecognized format for led message." << std::endl;
			return;
		    }
		}
		else {
		    std::cerr << "unrecognized format for led message." << std::endl;
		    return;
		}
		heartbeat_ = 5; // we just heard from the looper
	    }
	}
    }

    void handleDisplayMessage(const OSCMessage& message)
    {
	if (! message.isEmpty())
	{
	    OSCArgument* arg = message.begin();
	    int selectedLoop = -1;
	    if (arg->isInt32())
	    {
		selectedLoop = arg->getInt32() + 1; // 1 based display!
		++arg;
	    }
	    else {
		std::cerr << "unrecognized format for display message." << std::endl;
		return;
	    }

	    int wrote;
	    if (selectedLoop / 10 > 0)
	    {
		unsigned char ch[]={MIDI_CMD_CONTROL, 113, (unsigned char)(selectedLoop / 10)};
		wrote = snd_rawmidi_write(midiOut_, &ch, sizeof(ch));
		if (wrote != sizeof(ch))
		{
		    std::cerr << "Could not write CC " << (int)113 << " " << (int)(selectedLoop / 10) << std::endl;
		}
		//sendMidiMessage(midiOut_, MidiMessage::controllerEvent(channel_, 113, (uint8)(selectedLoop / 10)));
	    }
	    else
	    {
		unsigned char ch[]={MIDI_CMD_CONTROL, 113, 0};
		wrote = snd_rawmidi_write(midiOut_, &ch, sizeof(ch));
		if (wrote != sizeof(ch))
		{
		    std::cerr << "Could not write CC " << (int)113 << " " << 0 << std::endl;
		}
		//sendMidiMessage(midiOut_, MidiMessage::controllerEvent(channel_, 113, (uint8)0));
	    }

	    unsigned char ch[]={MIDI_CMD_CONTROL, 114, (unsigned char)(selectedLoop % 10)};
	    wrote = snd_rawmidi_write(midiOut_, &ch, sizeof(ch));
	    if (wrote != sizeof(ch))
	    {
		std::cerr << "Could not write CC " << (int)113 << " " << (int)(selectedLoop % 10) << std::endl;
	    }
	    //sendMidiMessage(midiOut_, MidiMessage::controllerEvent(channel_, 114, (uint8)(selectedLoop % 10)));
	}
    }

    void oscMessageReceived (const OSCMessage& message) override
    {
	if (!message.getAddressPattern().toString().startsWith("/heartbeat"))
	{
	    std::cout << "-" <<
	    + "- osc message, address = '"
	    + message.getAddressPattern().toString()
	    + "', "
	    + String (message.size())
	    + " argument(s)" << std::endl;

	    for (OSCArgument* arg = message.begin(); arg != message.end(); ++arg)
	    {
		String typeAsString;
		String valueAsString;

		if (arg->isFloat32())
		{
		    typeAsString = "float32";
		    valueAsString = String (arg->getFloat32());
		}
		else if (arg->isInt32())
		{
		    typeAsString = "int32";
		    valueAsString = String (arg->getInt32());
		}
		else if (arg->isString())
		{
		    typeAsString = "string";
		    valueAsString = arg->getString();
		}
		else if (arg->isBlob())
		{
		    typeAsString = "blob";
		    auto& blob = arg->getBlob();
		    valueAsString = String::fromUTF8 ((const char*) blob.getData(), (int) blob.getSize());
		}
		else
		{
		    typeAsString = "(unknown)";
		}

		std::cout << "==- " + typeAsString.paddedRight(' ', 12) + valueAsString << std::endl;
	    }
	}

	if (message.getAddressPattern().toString().startsWith("/pingack"))
	{
	    handlePingAckMessage(message);
	}
	else if (message.getAddressPattern().toString().startsWith("/led"))
	{
	    handleLedMessage(message);
	}
	else if (message.getAddressPattern().toString().startsWith("/display"))
	{
	    handleDisplayMessage(message);
	}
	else if (message.getAddressPattern().toString().startsWith("/heartbeat"))
	{
	    handleHeartbeatMessage(message);
	}
    }

    void oscBundleReceived (const OSCBundle& bundle) override
    {

    }

    void connect()
    {
	auto portToConnect = oscReceivePort_;

	if (! isValidOscPort (portToConnect))
	{
	    handleInvalidPortNumberEntered();
	    return;
	}

	if (oscReceiver.connect (portToConnect))
	{
	    currentReceivePort_ = portToConnect;
	    oscReceiver.addListener (this);
	    oscReceiver.registerFormatErrorHandler ([this] (const char* data, int dataSize)
						    {
							std::cerr << "- (" + String(dataSize) + "bytes with invalid format)" << std::endl;
						    });
	}
	else
	{
	    handleConnectError (portToConnect);
	}
    }

    void disconnect()
    {
	if (oscReceiver.disconnect())
	{
	    currentReceivePort_ = -1;
	    oscReceiver.removeListener (this);
	}
	else
	{
	    handleDisconnectError();
	}
    }

    void handleConnectError (int failedPort)
    {
	std::cerr << "Error: could not connect to port " + String (failedPort) << std::endl;
    }

    void handleDisconnectError()
    {
	std::cerr << "An unknown error occured while trying to disconnect from UDP port." << std::endl;
    }

    void handleInvalidPortNumberEntered()
    {
	std::cout << "Error: you have entered an invalid UDP port number." << std::endl;
    }

    bool isConnected() const
    {
	return currentReceivePort_ != -1;
    }

    bool isValidOscPort (int port) const
    {
	return port > 0 && port < 65536;
    }

    void printVersion()
    {
	std::cout << ProjectInfo::projectName << " v" << ProjectInfo::versionString << std::endl;
	std::cout << "https://github.com/atinm/loop4r_control" << std::endl;
    }

    void printUsage()
    {
	printVersion();
	std::cout << std::endl;
	std::cout << "Usage: " << ProjectInfo::projectName << " [ commands ] [ programfile ] [ -- ]" << std::endl << std::endl
	<< "Commands:" << std::endl;
	for (auto&& cmd : commands_)
	{
	    std::cout << "  " << cmd.param_.paddedRight(' ', 5);
	    if (cmd.optionsDescription_.isNotEmpty())
	    {
		std::cout << " " << cmd.optionsDescription_.paddedRight(' ', 13);
	    }
	    else
	    {
		std::cout << "              ";
	    }
	    std::cout << "  " << cmd.commandDescription_;
	    std::cout << std::endl;
	}
	std::cout << "  -h  or  --help       Print Help (this message) and exit" << std::endl;
	std::cout << "  --version            Print version information and exit" << std::endl;
	std::cout << "  --                   Read commands from standard input until it's closed" << std::endl;
	std::cout << std::endl;
	std::cout << "Alternatively, you can use the following long versions of the commands:" << std::endl;
	String line = " ";
	for (auto&& cmd : commands_)
	{
	    if (cmd.altParam_.isNotEmpty())
	    {
		if (line.length() + cmd.altParam_.length() + 1 >= 80)
		{
		    std::cout << line << std::endl;
		    line = " ";
		}
		line << " " << cmd.altParam_;
	    }
	}
	std::cout << line << std::endl << std::endl;
	std::cout << "By default, numbers are interpreted in the decimal system, this can be changed" << std::endl
	<< "to hexadecimal by sending the \"hex\" command. Additionally, by suffixing a " << std::endl
	<< "number with \"M\" or \"H\", it will be interpreted as a decimal or hexadecimal" << std::endl
	<< "respectively." << std::endl;
	std::cout << std::endl;
	std::cout << "The MIDI device name doesn't have to be an exact match." << std::endl;
	std::cout << "If " << getApplicationName() << " can't find the exact name that was specified, it will pick the" << std::endl
	<< "first MIDI output port that contains the provided text, irrespective of case." << std::endl;
	std::cout << std::endl;
    }

    OSCReceiver oscReceiver;
    OSCSender oscSender;

    int currentReceivePort_ = -1;
    int currentSendPort_ = -1;
    int channel_;
    int oscSendPort_;
    int oscReceivePort_;
    int engineId_;

    Array<LED> leds_;
    Array<ApplicationCommand> commands_;
    Array<ApplicationCommand> filterCommands_;

    bool useHexadecimalsByDefault_;

    String midiOutName_;
    snd_rawmidi_t *midiOut_ = 0;

    int ledCount_;
    bool pinged_;
    String hostUrl_;
    String version_;
    int heartbeat_;
    bool heartbeatOn_ = false;

    ApplicationCommand currentCommand_;
    Time lastTime_;
};

//==============================================================================
// This macro generates the main() routine that launches the app.
START_JUCE_APPLICATION (loop4r_ledsApplication)
