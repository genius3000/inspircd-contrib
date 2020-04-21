/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2013 Daniel Vassdal <shutter@canternet.org>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* $ModAuthor: Daniel Vassdal */
/* $ModAuthorMail: shutter@canternet.org */
/* $ModDesc: Enables two factor authentication for oper blocks */
/* $ModDepends: core 2.0 */
/* $ModConfig: <totp hash="sha256" window="5"> */

/*
	Note:
	This module requires a SHA1 provider to work with Google Authenticator.
	Works with FreeOTP just fine with m_sha256

	Associate a secret generated by /totp to each oper block you want to
	activate this module on. When you generate it, a link to a QR code with the
	settings is provided for simplicity. Scan this with your OTP phone app.
*/

#include "inspircd.h"
#include "hash.h"

class Base32
{
	static const std::string Base32Chars;

public:
	static std::string Encode(const std::string& input, size_t len = 0)
	{
		if (!len)
			len = input.length();

		size_t blocks = std::floor(len / 5);
		size_t rest = len % 5;

		std::vector<unsigned char> data(input.begin(), input.end());
		data.resize(len);

		if (rest)
		{
			data.resize(data.size() +  5 - rest);
			++blocks;
		}

		std::string ret;
		for (size_t i = 0; i < blocks; ++i)
		{
			ret += Base32Chars[data[i*5] >> 3];
			ret += Base32Chars[(data[i * 5] & 0x07) << 2 | (data[i * 5 + 1] >> 6)];
			ret += Base32Chars[(data[i * 5 + 1] & 0x3f) >> 1];
			ret += Base32Chars[(data[i * 5 + 1] & 0x01) << 4 | (data[i * 5 + 2] >> 4)];
			ret += Base32Chars[(data[i * 5 + 2] & 0x0f) << 1 | (data[i * 5 + 3] >> 7)];
			ret += Base32Chars[(data[i * 5 + 3] & 0x7f) >> 2];
			ret += Base32Chars[(data[i * 5 + 3] & 0x03) << 3 | (data[i * 5 + 4] >> 5)];
			ret += Base32Chars[(data[i * 5 + 4] & 0x1f)];
		}

		short padding =
			rest == 1 ? 6 :
			rest == 2 ? 3 :
			rest == 3 ? 3 :
			rest == 4 ? 1 : 0;

		ret = ret.substr(0, ret.length() - padding);
		ret.append(padding, '=');
		return ret;
	}

	static std::string Decode(const std::string& data)
	{
		std::string ret;
		ret.resize((data.length() * 5) / 8);

		size_t left = 0;
		size_t count = 0;

		unsigned int buffer = 0;
		for (std::string::const_iterator it = data.begin(); it != data.end(); ++it)
		{
			size_t val = Base32Chars.find(*it);
			if (val >= 32)
				continue;

			buffer <<= 5;
			buffer |= val;
			left += 5;
			if (left >= 8)
			{
				ret[count++] = (buffer >> (left - 8)) & 0xff;
				left -= 8;
			}
		}

		if (left)
		{
			buffer <<= 5;
			ret[count++] = (buffer >> (left - 3)) & 0xff;
		}

		ret.resize(count);
		return ret;
	}
};
const 	std::string Base32::Base32Chars("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567");

class TOTP
{
 public:
	dynamic_reference<HashProvider>& Hash;
	int Window;

	TOTP(dynamic_reference<HashProvider>& hp) : Hash(hp), Window(5)
	{
	}

	std::string Generate(const std::string secret, unsigned long time = 0)
	{
		if (!Hash)
			return "";

		std::vector<uint8_t> challenge(8);
		for (int i = 8; i--; time >>= 8)
			challenge[i] = time;

		std::string key = Base32::Decode(secret);
		std::string hash = Hash->hmac(key, std::string(challenge.begin(), challenge.end()));

		int offset = hash[Hash->out_size - 1] & 0xF;
		unsigned int truncatedHash = 0;
		for (int i = 0; i < 4; ++i)
		{
			truncatedHash <<= 8;
			truncatedHash  |= (unsigned char)hash[offset + i];
		}

		truncatedHash &= 0x7FFFFFFF;
		truncatedHash %= 1000000;

		std::string ret = ConvToStr(truncatedHash);
		ret.insert(0, 6 - ret.length(), '0');
		return ret;
	}

	bool Validate(const std::string& secret, const std::string& code)
	{
		unsigned long time = (ServerInstance->Time() - 30 * Window) / 30;
		unsigned long time_end = (ServerInstance->Time() + 30 * Window) / 30;
		for (; time < time_end; ++time)
			if (Generate(secret, time) == code)
				return true;
		return false;
	}
};

class CommandTOTP : public Command
{
	TOTP& totp;

	void ShowCode(User* user, const std::string& secret, const std::string& label = "")
	{	
		std::string url = "https://www.google.com/chart?chs=200x200&chld=M|0&cht=qr&chl=otpauth%3A%2F%2Ftotp%2F"
			+ ServerInstance->Config->Network + (!label.empty() ? "%20(" + label + ")" : "") + "%3Falgorithm%3D"
			+ totp.Hash->name.substr(5) + "%26secret%3D" + secret;

		user->WriteServ("NOTICE %s :Secret: %s", user->nick.c_str(), secret.c_str());
		user->WriteServ("NOTICE %s :Algorithm: %s", user->nick.c_str(), totp.Hash->name.substr(5).c_str());
		user->WriteServ("NOTICE %s :QR Code: %s", user->nick.c_str(),  + url.c_str());
	}

	void GenerateCode(User* user, const std::string& label = "")
	{
		std::string secret;
		secret.resize(10);
		for (unsigned char i = 0; i < 10; ++i)
			secret[i] = (unsigned char)ServerInstance->GenRandomInt(0xff);

		user->WriteServ("NOTICE %s :Generated TOTP%s %s:", user->nick.c_str(), (!label.empty() ? " for" : ""), label.c_str());
		ShowCode(user, Base32::Encode(secret, 10), label);
	}

public:
	CommandTOTP(Module* Creator, TOTP& to) : Command(Creator, "TOTP", 0), totp(to)
	{
		syntax = "<label|code>";
		flags_needed = 'o';
	}

	CmdResult Handle (const std::vector<std::string>& parameters, User *user)
	{
		if (!totp.Hash)
		{
			user->WriteServ("NOTICE %s :The TOTP hash provider specified is not loaded.", user->nick.c_str());
			return CMD_SUCCESS;
		}

		if (parameters.empty())
		{
			GenerateCode(user);
			return CMD_SUCCESS;
		}

		if (parameters[0].length() == 6 && ConvToInt(parameters[0]))
		{
			std::string secret;
			if (!user->oper->oper_block->readString("totpsecret", secret))
				return CMD_SUCCESS;

			if (!totp.Validate(secret, parameters[0]))
			{
				user->WriteServ("NOTICE %s :TOTP not valid: %s", user->nick.c_str(), parameters[0].c_str());
				return CMD_FAILURE;
			}

			std::string uname;
			user->oper->oper_block->readString("name", uname);
			user->WriteServ("NOTICE %s :Fetched your TOTP secret from config:", user->nick.c_str());
			ShowCode(user, secret, uname);
		}
		else
			GenerateCode(user, parameters[0]);

		return CMD_SUCCESS;
	}
};


class ModuleTOTP : public Module
{
	dynamic_reference<HashProvider> Hash;
	TOTP totp;
	CommandTOTP cmd;

 public:
	ModuleTOTP() : Hash(this, "hash/sha256"), totp(Hash), cmd(this, totp)
	{
	}

	void init()
	{
		ServerInstance->Modules->AddService(cmd);
		Implementation eventlist[] = { I_OnPreCommand, I_OnRehash };
		ServerInstance->Modules->Attach(eventlist, this, sizeof(eventlist) / sizeof(Implementation));

		OnRehash(NULL);
	}

	void OnRehash(User* user)
	{
		ConfigTag* tag = ServerInstance->Config->ConfValue("totp");
		totp.Window = tag->getInt("window", 5);
		Hash.SetProvider("hash/" + tag->getString("hash", "sha256"));
	}

	ModResult OnPreCommand(std::string &command, std::vector<std::string> &parameters, LocalUser *user, bool validated, const std::string &original_line)
	{
		if (validated && command == "OPER" && parameters.size() > 1)
		{
			OperIndex::iterator it = ServerInstance->Config->oper_blocks.find(parameters[0]);
			if (it == ServerInstance->Config->oper_blocks.end())
				return MOD_RES_PASSTHRU;

			OperInfo* info = it->second;

			std::string secret;
			if (!info->oper_block->readString("totpsecret", secret))
				return MOD_RES_PASSTHRU;

			size_t pos = parameters[1].rfind(' ');
			if (pos == std::string::npos)
			{
				user->WriteNumeric(491, "%s :This oper login requires a TOTP token.", user->nick.c_str());
				return MOD_RES_DENY;
			}

			std::string otp = parameters[1].substr(pos + 1);
			parameters[1].erase(pos);

			if (totp.Validate(secret, otp))
				return MOD_RES_PASSTHRU;

			user->WriteNumeric(491, "%s :Invalid oper credentials",user->nick.c_str());
			user->CommandFloodPenalty += 10000;
			return MOD_RES_DENY;			
		}

		return MOD_RES_PASSTHRU;
	}

	Version GetVersion()
	{
		return Version("Enables two factor authentication for oper blocks");
	}
};

MODULE_INIT(ModuleTOTP)
