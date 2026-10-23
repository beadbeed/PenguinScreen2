// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#include "VR/VrPresence.h"
#include "VR/NetGate.h"

#include "VR/VRInputState.h"
#include "VR/VRProfileDB.h"
#include "Config.h"
#include "Memory.h"
#include "VMManager.h"
#include "common/Console.h"
#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace VR::Presence
{
	namespace
	{
		struct V3
		{
			double x = 0.0, y = 0.0, z = 0.0;
		};
		V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
		V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
		V3 operator*(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
		double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		V3 Cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
		double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
		V3 Load(const float v[3]) { return {v[0], v[1], v[2]}; }
		void Store(const V3& a, float out[3])
		{
			out[0] = static_cast<float>(a.x);
			out[1] = static_cast<float>(a.y);
			out[2] = static_cast<float>(a.z);
		}
		bool Finite(const V3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
		bool Finite3(const float v[3]) { return v && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
		bool Normalize(V3& a)
		{
			const double l = Len(a);
			if (!(l > 1.0e-9) || !std::isfinite(l))
				return false;
			a = a * (1.0 / l);
			return true;
		}

		// v rotated by the unit quaternion q (xyzw).
		V3 Rotate(const float q[4], const V3& v)
		{
			const V3 u{q[0], q[1], q[2]};
			const double s = q[3];
			return u * (2.0 * Dot(u, v)) + v * (s * s - Dot(u, u)) + Cross(u, v) * (2.0 * s);
		}

		bool UnitQuat(const float in[4], float out[4])
		{
			const double l = std::sqrt(static_cast<double>(in[0]) * in[0] + static_cast<double>(in[1]) * in[1] +
									   static_cast<double>(in[2]) * in[2] + static_cast<double>(in[3]) * in[3]);
			if (!(l > 1.0e-6) || !std::isfinite(l))
				return false;
			for (int k = 0; k < 4; k++)
				out[k] = static_cast<float>(in[k] / l);
			return true;
		}

		// The rotation whose columns are x, y, z (unit, perpendicular, right-handed) as a quaternion xyzw.
		void QuatFromAxes(const V3& x, const V3& y, const V3& z, float q[4])
		{
			const double m00 = x.x, m01 = y.x, m02 = z.x;
			const double m10 = x.y, m11 = y.y, m12 = z.y;
			const double m20 = x.z, m21 = y.z, m22 = z.z;
			const double tr = m00 + m11 + m22;
			double qx, qy, qz, qw;
			if (tr > 0.0)
			{
				const double s = std::sqrt(tr + 1.0) * 2.0;
				qw = 0.25 * s;
				qx = (m21 - m12) / s;
				qy = (m02 - m20) / s;
				qz = (m10 - m01) / s;
			}
			else if (m00 > m11 && m00 > m22)
			{
				const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
				qw = (m21 - m12) / s;
				qx = 0.25 * s;
				qy = (m01 + m10) / s;
				qz = (m02 + m20) / s;
			}
			else if (m11 > m22)
			{
				const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
				qw = (m02 - m20) / s;
				qx = (m01 + m10) / s;
				qy = 0.25 * s;
				qz = (m12 + m21) / s;
			}
			else
			{
				const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
				qw = (m10 - m01) / s;
				qx = (m02 + m20) / s;
				qy = (m12 + m21) / s;
				qz = 0.25 * s;
			}
			const double l = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
			q[0] = static_cast<float>(qx / l);
			q[1] = static_cast<float>(qy / l);
			q[2] = static_cast<float>(qz / l);
			q[3] = static_cast<float>(qw / l);
		}

		// An orientation from a forward (-Z) and an up (+Y) direction, up made perpendicular to forward.
		bool QuatFromForwardUp(V3 fwd, V3 up, float q[4])
		{
			if (!Normalize(fwd))
				return false;
			up = up - fwd * Dot(up, fwd);
			if (!Normalize(up))
				return false;
			const V3 back = fwd * -1.0;
			const V3 right = Cross(up, back);
			QuatFromAxes(right, up, back, q);
			return true;
		}

		// --- The wire format -------------------------------------------------------------------------------------------
		u32 Fnv1a(const u8* data, size_t n)
		{
			u32 h = 2166136261u;
			for (size_t i = 0; i < n; i++)
			{
				h ^= data[i];
				h *= 16777619u;
			}
			return h;
		}

		struct Writer
		{
			u8* p;
			size_t at = 0;
			void U8(u8 v) { p[at++] = v; }
			void U16(u16 v)
			{
				U8(static_cast<u8>(v));
				U8(static_cast<u8>(v >> 8));
			}
			void U32(u32 v)
			{
				U16(static_cast<u16>(v));
				U16(static_cast<u16>(v >> 16));
			}
			void F32(float f) { U32(std::bit_cast<u32>(f)); }
			void F3(const float v[3])
			{
				for (int k = 0; k < 3; k++)
					F32(v[k]);
			}
		};

		struct Reader
		{
			const u8* p;
			size_t at = 0;
			u8 U8() { return p[at++]; }
			u16 U16()
			{
				const u16 lo = U8();
				return static_cast<u16>(lo | (static_cast<u16>(U8()) << 8));
			}
			u32 U32()
			{
				const u32 lo = U16();
				return lo | (static_cast<u32>(U16()) << 16);
			}
			float F32() { return std::bit_cast<float>(U32()); }
			void F3(float v[3])
			{
				for (int k = 0; k < 3; k++)
					v[k] = F32();
			}
		};

		bool PoseOk(const GamePose& g)
		{
			if (!Finite3(g.pos) || !Finite3(g.fwd) || !Finite3(g.up))
				return false;
			for (int k = 0; k < 3; k++)
			{
				if (std::fabs(g.pos[k]) > 1.0e6f)
					return false;
			}
			const V3 f = Load(g.fwd), u = Load(g.up);
			const double lf = Len(f), lu = Len(u);
			return lf > 0.9 && lf < 1.1 && lu > 0.9 && lu < 1.1 && std::fabs(Dot(f, u)) < 0.2;
		}

		// --- The shared state ------------------------------------------------------------------------------------------
		struct NetConfig
		{
			bool valid = false;
			std::string host;
			bool numeric = false;
			u32 addr = 0; // with numeric
			u16 port = 0;
			u16 local_port = 0;
			u32 key = 0;
		};

		std::mutex s_mutex; // everything below up to s_game
		NetConfig s_cfg;
		u64 s_cfg_gen = 0;
		Packet s_out;
		bool s_out_valid = false;
		u64 s_out_ms = 0;
		Packet s_in;
		bool s_in_valid = false;
		u64 s_in_ms = 0;

		std::mutex s_view_mutex;
		PartnerGame s_game;

		std::atomic<bool> s_active{false};
		std::atomic<bool> s_wanted{false};
		std::atomic<bool> s_stop{false};
		std::unique_ptr<std::thread> s_thread;

		std::atomic<u32> s_sent{0}, s_received{0}, s_foreign{0}, s_bad{0}, s_wrong_key{0}, s_stale{0};

		u64 NowMs()
		{
			return CameraDriver::SteadyNowMs();
		}

		void SleepMs(int ms)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		}

		std::string Ip(u32 a)
		{
			return fmt::format("{}.{}.{}.{}", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
		}

#ifdef _WIN32
		using Sock = SOCKET;
		constexpr Sock kNoSock = INVALID_SOCKET;
		void CloseSock(Sock s) { closesocket(s); }
#else
		using Sock = int;
		constexpr Sock kNoSock = -1;
		void CloseSock(Sock s) { close(s); }
#endif

		// The local IPv4 address (host order) the system routes to peer from: a UDP connect() sends nothing, it only picks
		// the route and the source address. 0 when there is none.
		u32 LocalAddressFor(u32 peer, u16 port)
		{
			Sock t = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (t == kNoSock)
				return 0;
			sockaddr_in to = {};
			to.sin_family = AF_INET;
			to.sin_port = htons(port);
			to.sin_addr.s_addr = htonl(peer);
			u32 out = 0;
			if (connect(t, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0)
			{
				sockaddr_in me = {};
#ifdef _WIN32
				int len = sizeof(me);
#else
				socklen_t len = sizeof(me);
#endif
				if (getsockname(t, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_family == AF_INET)
					out = ntohl(me.sin_addr.s_addr);
			}
			CloseSock(t);
			return out;
		}

		// The socket thread: opens the socket only while Poll says the channel is live (closed again 2 s after it is not),
		// sends the newest local pose every kSendIntervalMs and keeps the newest accepted packet from the peer.
		void NetThread()
		{
#ifdef _WIN32
			WSADATA wsa;
			const bool wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
			if (!wsa_ok)
			{
				Console.Error("(VR) Presence: WSAStartup failed; no VR presence this session.");
				return;
			}
#endif
			Sock sock = kNoSock;
			u64 open_gen = 0;
			u64 refused_gen = 0;
			u32 peer_addr = 0;
			u16 peer_port = 0;
			u64 inactive_since = 0;
			u64 last_send = 0;
			std::random_device rd;
			const u32 session = rd() ^ static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count());
			u32 seq = 0;
			SeqState seqst;
			while (!s_stop.load(std::memory_order_acquire))
			{
				const u64 now = NowMs();
				NetConfig cfg;
				u64 gen;
				{
					std::lock_guard<std::mutex> lock(s_mutex);
					cfg = s_cfg;
					gen = s_cfg_gen;
				}
				const bool active = s_active.load(std::memory_order_acquire);
				if (active)
					inactive_since = 0;
				else if (inactive_since == 0)
					inactive_since = now;
				const bool keep = cfg.valid && (active || (inactive_since != 0 && now - inactive_since < 2000));
				if (sock != kNoSock && (!keep || gen != open_gen))
				{
					CloseSock(sock);
					sock = kNoSock;
					Console.WriteLn("(VR) Presence: channel closed (%s); %u sent, %u received, dropped %u foreign, %u bad, %u "
									"wrong key, %u stale.",
						gen != open_gen ? "the settings changed" : "first person is off or not in private co-op",
						s_sent.load(), s_received.load(), s_foreign.load(), s_bad.load(), s_wrong_key.load(), s_stale.load());
					std::lock_guard<std::mutex> lock(s_mutex);
					s_in_valid = false;
				}
				if (!keep || (sock == kNoSock && (!active || refused_gen == gen)))
				{
					SleepMs(50);
					continue;
				}
				if (sock == kNoSock)
				{
					u32 addr = cfg.numeric ? cfg.addr : 0;
					if (!cfg.numeric)
					{
						addrinfo hints = {};
						hints.ai_family = AF_INET;
						hints.ai_socktype = SOCK_DGRAM;
						addrinfo* res = nullptr;
						if (getaddrinfo(cfg.host.c_str(), nullptr, &hints, &res) == 0 && res)
						{
							for (addrinfo* r = res; r; r = r->ai_next)
							{
								if (r->ai_family == AF_INET && r->ai_addrlen >= sizeof(sockaddr_in))
								{
									addr = ntohl(reinterpret_cast<const sockaddr_in*>(r->ai_addr)->sin_addr.s_addr);
									break;
								}
							}
							freeaddrinfo(res);
						}
					}
					if (addr == 0 || !IsPrivateIPv4(addr))
					{
						refused_gen = gen;
						if (addr == 0)
							Console.Warning("(VR) Presence: VrPresencePeer %s:%u does not resolve to an IPv4 address; nothing is "
											"sent.", cfg.host.c_str(), cfg.port);
						else
							Console.Warning("(VR) Presence: VrPresencePeer %s:%u is %s, not a private address (LAN, Tailscale "
											"100.64/10 or this PC); refused, nothing is sent.",
								cfg.host.c_str(), cfg.port, Ip(addr).c_str());
						continue;
					}
					Sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
					if (s == kNoSock)
					{
						refused_gen = gen;
						Console.Error("(VR) Presence: socket() failed; nothing is sent.");
						continue;
					}
					// Listen only on the interface that routes to the peer (night review m5: never 0.0.0.0, so not on every
					// network the PC is on): loopback for a loopback peer, else the local address a UDP connect() to the
					// peer picks.
					const u32 local_addr = IsLoopbackIPv4(addr) ? 0x7F000001u : LocalAddressFor(addr, cfg.port);
					if (local_addr == 0)
					{
						CloseSock(s);
						refused_gen = gen;
						Console.Error("(VR) Presence: no local interface routes to %s; nothing is sent.", Ip(addr).c_str());
						continue;
					}
					sockaddr_in local = {};
					local.sin_family = AF_INET;
					local.sin_port = htons(cfg.local_port);
					local.sin_addr.s_addr = htonl(local_addr);
					if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
					{
						CloseSock(s);
						refused_gen = gen;
						Console.Error("(VR) Presence: port %u is taken (bind failed); nothing is sent. Set [VR] VrPresencePort.",
							cfg.local_port);
						continue;
					}
#ifdef _WIN32
					u_long nb = 1;
					ioctlsocket(s, FIONBIO, &nb);
					// A send to a port nobody listens on must not fail the next receive.
					BOOL reset = FALSE;
					DWORD ret = 0;
					WSAIoctl(s, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &ret, nullptr, nullptr);
#else
					fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
					sock = s;
					open_gen = gen;
					peer_addr = addr;
					peer_port = cfg.port;
					seqst = SeqState{};
					Console.WriteLn("(VR) Presence: channel open: listening on %s:%u, sending to %s:%u (%s), about %u packets "
									"a second of %u bytes.",
						Ip(local_addr).c_str(), cfg.local_port, Ip(addr).c_str(), cfg.port,
						cfg.host.c_str(), 1000u / kSendIntervalMs, static_cast<unsigned>(kPacketSize));
				}

				// Receive, waiting at most 10 ms.
				fd_set rs;
				FD_ZERO(&rs);
				FD_SET(sock, &rs);
				timeval tv = {0, 10000};
#ifdef _WIN32
				const int ready = select(0, &rs, nullptr, nullptr, &tv);
#else
				const int ready = select(sock + 1, &rs, nullptr, nullptr, &tv);
#endif
				if (ready > 0)
				{
					for (int i = 0; i < 64; i++)
					{
						u8 buf[512];
						sockaddr_in from = {};
#ifdef _WIN32
						int from_len = sizeof(from);
						const int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
							reinterpret_cast<sockaddr*>(&from), &from_len);
#else
						socklen_t from_len = sizeof(from);
						const ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
						if (n < 0)
							break;
						if (from.sin_family != AF_INET || ntohl(from.sin_addr.s_addr) != peer_addr || ntohs(from.sin_port) != peer_port)
						{
							s_foreign.fetch_add(1, std::memory_order_relaxed);
							continue;
						}
						Packet p;
						if (!Decode(buf, static_cast<size_t>(n), &p))
						{
							s_bad.fetch_add(1, std::memory_order_relaxed);
							continue;
						}
						if (p.key != cfg.key)
						{
							s_wrong_key.fetch_add(1, std::memory_order_relaxed);
							continue;
						}
						const u64 rx = NowMs();
						if (!Accept(seqst, p.session, p.seq, rx))
						{
							s_stale.fetch_add(1, std::memory_order_relaxed);
							continue;
						}
						s_received.fetch_add(1, std::memory_order_relaxed);
						std::lock_guard<std::mutex> lock(s_mutex);
						s_in = p;
						s_in_valid = true;
						s_in_ms = rx;
					}
				}

				// Send the newest local pose, if it is fresh and the channel is live.
				const u64 t = NowMs();
				if (active && t - last_send >= kSendIntervalMs)
				{
					Packet p;
					bool have = false;
					{
						std::lock_guard<std::mutex> lock(s_mutex);
						if (s_out_valid && t - s_out_ms <= 100)
						{
							p = s_out;
							have = true;
						}
					}
					if (have)
					{
						p.key = cfg.key;
						p.session = session;
						p.seq = ++seq;
						p.send_ms = static_cast<u32>(t);
						u8 buf[kPacketSize];
						if (Encode(p, buf, sizeof(buf)))
						{
							sockaddr_in to = {};
							to.sin_family = AF_INET;
							to.sin_port = htons(peer_port);
							to.sin_addr.s_addr = htonl(peer_addr);
							if (sendto(sock, reinterpret_cast<const char*>(buf), static_cast<int>(kPacketSize), 0,
									reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == static_cast<int>(kPacketSize))
								s_sent.fetch_add(1, std::memory_order_relaxed);
						}
						last_send = t;
					}
				}
			}
			if (sock != kNoSock)
				CloseSock(sock);
#ifdef _WIN32
			WSACleanup();
#endif
		}

		void StartNet()
		{
			if (s_thread)
				return;
			s_stop.store(false, std::memory_order_release);
			s_thread = std::make_unique<std::thread>(NetThread);
			static bool s_atexit = false;
			if (!s_atexit)
			{
				s_atexit = true;
				std::atexit([]() { Shutdown(); });
			}
		}

		void ClearView()
		{
			std::lock_guard<std::mutex> lock(s_view_mutex);
			s_game = PartnerGame{};
		}

		// --- The CPU thread's view of the peer setting -----------------------------------------------------------------
		std::string s_peer_text;
		int s_port_setting = -1;
		std::string s_key_text;
		bool s_cfg_ok = false;
	} // namespace

	// --- Wire format ---------------------------------------------------------------------------------------------------
	bool Encode(const Packet& p, u8* out, size_t size)
	{
		if (!out || size < kPacketSize)
			return false;
		Writer w{out};
		w.U32(kMagic);
		w.U8(kVersion);
		w.U8(p.flags);
		w.U8(p.slot);
		w.U8(0);
		w.U16(p.room);
		w.U8(p.grip[0]);
		w.U8(p.grip[1]);
		w.U32(p.key);
		w.U32(p.session);
		w.U32(p.seq);
		w.U32(p.send_ms);
		w.F3(p.character);
		for (const GamePose* g : {&p.head, &p.hand[0], &p.hand[1]})
		{
			w.F3(g->pos);
			w.F3(g->fwd);
			w.F3(g->up);
		}
		w.U8(p.trigger[0]);
		w.U8(p.trigger[1]);
		w.U16(0);
		const u32 sum = Fnv1a(out, w.at);
		w.U32(sum);
		return w.at == kPacketSize;
	}

	bool Decode(const u8* data, size_t size, Packet* out)
	{
		if (!data || !out || size != kPacketSize)
			return false;
		Reader r{data};
		if (r.U32() != kMagic || r.U8() != kVersion)
			return false;
		Packet p;
		p.flags = r.U8();
		p.slot = r.U8();
		r.U8();
		p.room = r.U16();
		p.grip[0] = r.U8();
		p.grip[1] = r.U8();
		p.key = r.U32();
		p.session = r.U32();
		p.seq = r.U32();
		p.send_ms = r.U32();
		r.F3(p.character);
		for (GamePose* g : {&p.head, &p.hand[0], &p.hand[1]})
		{
			r.F3(g->pos);
			r.F3(g->fwd);
			r.F3(g->up);
		}
		p.trigger[0] = r.U8();
		p.trigger[1] = r.U8();
		r.U16();
		const size_t body = r.at;
		if (r.U32() != Fnv1a(data, body))
			return false;
		if (p.slot > 3 || (p.flags & ~(kFlagHead | kFlagLeft | kFlagRight)) != 0 || !Finite3(p.character))
			return false;
		for (int k = 0; k < 3; k++)
		{
			if (std::fabs(p.character[k]) > 1.0e6f)
				return false;
		}
		if (((p.flags & kFlagHead) && !PoseOk(p.head)) || ((p.flags & kFlagLeft) && !PoseOk(p.hand[0])) ||
			((p.flags & kFlagRight) && !PoseOk(p.hand[1])))
			return false;
		*out = p;
		return true;
	}

	u32 KeyHash(std::string_view text)
	{
		return Fnv1a(reinterpret_cast<const u8*>(text.data()), text.size());
	}

	bool KeyUsable(std::string_view text)
	{
		for (char c : text)
		{
			if (c != ' ' && c != '	')
				return true;
		}
		return false;
	}

	bool IsLoopbackIPv4(u32 a)
	{
		return (a >> 24) == 127;
	}

	bool IsPrivateIPv4(u32 a)
	{
		const u32 b0 = a >> 24, b1 = (a >> 16) & 0xFF;
		return b0 == 10 || (b0 == 172 && (b1 & 0xF0) == 16) || (b0 == 192 && b1 == 168) || (b0 == 100 && (b1 & 0xC0) == 64) ||
			   b0 == 127;
	}

	bool ParseIPv4(std::string_view t, u32* addr)
	{
		u32 out = 0;
		int parts = 0;
		size_t i = 0;
		while (parts < 4)
		{
			if (i >= t.size() || t[i] < '0' || t[i] > '9')
				return false;
			u32 v = 0;
			int digits = 0;
			while (i < t.size() && t[i] >= '0' && t[i] <= '9')
			{
				v = v * 10 + static_cast<u32>(t[i] - '0');
				if (++digits > 3 || v > 255)
					return false;
				i++;
			}
			out = (out << 8) | v;
			parts++;
			if (parts < 4)
			{
				if (i >= t.size() || t[i] != '.')
					return false;
				i++;
			}
		}
		if (i != t.size())
			return false;
		if (addr)
			*addr = out;
		return true;
	}

	bool ParsePeer(std::string_view t, std::string* host, u16* port)
	{
		while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
			t.remove_prefix(1);
		while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
			t.remove_suffix(1);
		const size_t colon = t.rfind(':');
		if (colon == std::string_view::npos || colon == 0 || colon + 1 >= t.size() || t.find(':') != colon)
			return false;
		const std::string_view h = t.substr(0, colon), ps = t.substr(colon + 1);
		for (const char c : h)
		{
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
				return false;
		}
		if (h.size() > 253 || ps.size() > 5)
			return false;
		u32 v = 0;
		for (const char c : ps)
		{
			if (c < '0' || c > '9')
				return false;
			v = v * 10 + static_cast<u32>(c - '0');
		}
		if (v < 1 || v > 65535)
			return false;
		if (host)
			*host = std::string(h);
		if (port)
			*port = static_cast<u16>(v);
		return true;
	}

	bool Accept(SeqState& st, u32 session, u32 seq, u64 now_ms)
	{
		bool ok;
		if (!st.have)
			ok = true;
		else if (session == st.session)
			ok = static_cast<s32>(seq - st.seq) > 0;
		else
			ok = now_ms >= st.last_ms && now_ms - st.last_ms >= kSessionSwitchMs;
		if (!ok)
			return false;
		st.have = true;
		st.session = session;
		st.seq = seq;
		st.last_ms = now_ms;
		return true;
	}

	// --- Mapping -------------------------------------------------------------------------------------------------------
	bool PoseToGame(const CameraDriver::Mapping& m, const float pos[3], const float quat[4], GamePose* out)
	{
		float q[4];
		if (!out || !Finite3(pos) || !quat || !UnitQuat(quat, q))
			return false;
		const V3 p = Load(pos);
		const V3 f = Rotate(q, V3{0.0, 0.0, -1.0});
		const V3 u = Rotate(q, V3{0.0, 1.0, 0.0});
		float pf[3], pu[3], g[3], gf[3], gu[3];
		Store(p + f * 0.1, pf);
		Store(p + u * 0.1, pu);
		if (!CameraDriver::GameFromVr(m, pos, g) || !CameraDriver::GameFromVr(m, pf, gf) || !CameraDriver::GameFromVr(m, pu, gu))
			return false;
		V3 df = Load(gf) - Load(g), du = Load(gu) - Load(g);
		if (!Normalize(df) || !Normalize(du))
			return false;
		GamePose r;
		for (int k = 0; k < 3; k++)
			r.pos[k] = g[k];
		Store(df, r.fwd);
		Store(du, r.up);
		*out = r;
		return true;
	}

	bool PoseFromGame(const CameraDriver::Mapping& m, const GamePose& in, const float shift[3], float pos[3], float quat[4])
	{
		if (!pos || !quat || !Finite3(in.pos) || !Finite3(in.fwd) || !Finite3(in.up) || !std::isfinite(m.units_per_meter) ||
			!(std::fabs(m.units_per_meter) > 1.0e-3f))
			return false;
		const V3 s = shift ? Load(shift) : V3{};
		if (!Finite(s))
			return false;
		const double step = 0.1 * std::fabs(m.units_per_meter);
		V3 fd = Load(in.fwd), ud = Load(in.up);
		if (!Normalize(fd) || !Normalize(ud))
			return false;
		const V3 p = Load(in.pos) + s;
		float gp[3], gf[3], gu[3], v[3], vf[3], vu[3];
		Store(p, gp);
		Store(p + fd * step, gf);
		Store(p + ud * step, gu);
		if (!CameraDriver::VrFromGame(m, gp, v) || !CameraDriver::VrFromGame(m, gf, vf) || !CameraDriver::VrFromGame(m, gu, vu))
			return false;
		float q[4];
		if (!QuatFromForwardUp(Load(vf) - Load(v), Load(vu) - Load(v), q))
			return false;
		for (int k = 0; k < 3; k++)
			pos[k] = v[k];
		for (int k = 0; k < 4; k++)
			quat[k] = q[k];
		return true;
	}

	// --- The partner for the compositor --------------------------------------------------------------------------------
	bool GetPartnerView(PartnerView*)
	{
		return false; // nothing is drawn for the partner any more (see PlacePartner below)
	}

	bool Wanted()
	{
		return s_wanted.load(std::memory_order_acquire);
	}

	// The placeholder drawing (a grey head shell and teal CC0 hands in one impostor) is gone: VR body ([VR] VrBody) poses
	// the partner's own character instead. These stay only so the compositor's old partner slot still links; with
	// GetPartnerView always false it never makes the swapchain or draws.
	bool PlacePartner(const PartnerView&, const float[3], ZoneMarkers::Quad*) { return false; }
	void RasterPartner(const PartnerView&, const float[3], const ZoneMarkers::Quad&, std::vector<u32>& out_rgba) { out_rgba.clear(); }
	int PartnerMarks(const PartnerView&, ZoneMarkers::Mark*) { return 0; }

	// --- The partner for VR body ---------------------------------------------------------------------------------------
	bool GetPartnerGame(PartnerGame* out)
	{
		PartnerGame g;
		{
			std::lock_guard<std::mutex> lock(s_view_mutex);
			g = s_game;
		}
		const u64 now = NowMs();
		if (!g.valid || g.flags == 0 || (now > g.stamp_ms && now - g.stamp_ms > kStaleMs))
			return false;
		if (out)
			*out = g;
		return true;
	}

	// --- The CPU thread ------------------------------------------------------------------------------------------------
	void Shutdown()
	{
		s_active.store(false, std::memory_order_release);
		if (s_thread)
		{
			s_stop.store(true, std::memory_order_release);
			if (s_thread->joinable())
				s_thread->join();
			s_thread.reset();
		}
		ClearView();
	}

	void Poll()
	{
		const Pcsx2Config::VROptions& vr = EmuConfig.VR;
		const bool want = vr.VrPresence && !vr.VrPresencePeer.empty();
		s_wanted.store(want, std::memory_order_release);
		if (!want)
		{
			if (s_thread)
			{
				Console.WriteLn("(VR) Presence: off.");
				Shutdown();
			}
			s_peer_text.clear();
			return;
		}

		// The peer setting, read again only when it changes.
		if (vr.VrPresencePeer != s_peer_text || vr.VrPresencePort != s_port_setting || vr.VrPresenceKey != s_key_text)
		{
			s_peer_text = vr.VrPresencePeer;
			s_port_setting = vr.VrPresencePort;
			s_key_text = vr.VrPresenceKey;
			NetConfig cfg;
			std::string host;
			u16 port = 0;
			const bool port_ok = vr.VrPresencePort >= 1024 && vr.VrPresencePort <= 65535;
			s_cfg_ok = false;
			if (!ParsePeer(s_peer_text, &host, &port))
				Console.Warning("(VR) Presence: VrPresencePeer '%s' is not host:port; VR presence stays off.", s_peer_text.c_str());
			else if (!KeyUsable(s_key_text))
				Console.Warning("(VR) Presence: VrPresenceKey is empty; set the same word on both consoles. VR presence stays off.");
			else if (!port_ok)
				Console.Warning("(VR) Presence: VrPresencePort %d is not 1024-65535; VR presence stays off.", vr.VrPresencePort);
			else
			{
				u32 addr = 0;
				cfg.numeric = ParseIPv4(host, &addr);
				if (cfg.numeric && !IsPrivateIPv4(addr))
					Console.Warning("(VR) Presence: VrPresencePeer %s is not a private address (a LAN 10/8, 172.16/12, "
									"192.168/16, Tailscale 100.64/10, or this PC 127/8); refused, VR presence stays off.",
						host.c_str());
				else
				{
					cfg.valid = true;
					cfg.host = host;
					cfg.addr = addr;
					cfg.port = port;
					cfg.local_port = static_cast<u16>(vr.VrPresencePort);
					cfg.key = KeyHash(s_key_text);
					s_cfg_ok = true;
					Console.WriteLn("(VR) Presence: on, peer %s:%u, listening port %u; it goes live only with first person "
									"armed in private co-op%s.",
						host.c_str(), port, cfg.local_port,
						(cfg.numeric && IsLoopbackIPv4(addr)) ? " (or the offline loopback test, PCSX2_VR_PRESENCE_LOOPBACK=1)" : "");
				}
			}
			std::lock_guard<std::mutex> lock(s_mutex);
			s_cfg = cfg;
			s_cfg_gen++;
			s_in_valid = false;
		}
		if (!s_cfg_ok)
		{
			s_active.store(false, std::memory_order_release);
			ClearView();
			return;
		}
		StartNet();

		// Live only with first person armed (its record resolved and the head mapping published this vsync), and only in
		// private co-op, or offline for the loopback test with a 127/8 peer.
		static const bool s_loopback_env = [] {
			const char* e = std::getenv("PCSX2_VR_PRESENCE_LOOPBACK");
			return e && e[0] == '1';
		}();
		static const bool s_log_env = [] {
			const char* e = std::getenv("PCSX2_VR_PRESENCE_LOG");
			return e && e[0] == '1';
		}();
		NetConfig cfg;
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			cfg = s_cfg;
		}
		const bool loopback_test = s_loopback_env && cfg.numeric && IsLoopbackIPv4(cfg.addr) && CameraDriver::NetworkOffline();
		// Under the PCSX2_VR_TEST_NET test override (adapter off, private co-op paths selected) only a 127/8 peer: the
		// override never sends anything off this PC.
		const bool test_net_ok = !NetGate::TestOverrideActive() || (cfg.numeric && IsLoopbackIPv4(cfg.addr));
		const bool mode_ok = (CameraDriver::PrivateCoop() && test_net_ok) || loopback_test;

		const ProfileDB::Profile* profile = nullptr;
		if (mode_ok && VMManager::GetState() == VMState::Running)
		{
			const std::string serial = VMManager::GetDiscSerial();
			if (!serial.empty())
				profile = ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		}
		const ProfileDB::PresenceParams* pp =
			(profile && profile->presence.has_value()) ? &profile->presence.value() : nullptr;
		u32 base = 0;
		CameraDriver::Mapping m;
		const bool armed = pp && CameraDriver::LocalRecord(&base) && CameraDriver::GetMapping(&m);
		const bool active = mode_ok && armed;
		static int s_why = -1;
		const int why = !mode_ok ? 1 : !pp ? 2 : !armed ? 3 : 0;
		if (why != s_why)
		{
			s_why = why;
			static const char* const kWhy[] = {"live", "waiting: not in private co-op (nothing is sent)",
				"waiting: this game's profile has no presence block", "waiting: first person is not armed"};
			Console.WriteLn("(VR) Presence: %s%s.", kWhy[why], (why == 0 && loopback_test) ? " (offline loopback test)" : "");
		}
		s_active.store(active, std::memory_order_release);
		if (!active)
		{
			ClearView();
			return;
		}

		const ProfileDB::CameraBase& cb = profile->camera->base.value();
		const ProfileDB::CameraLookAt& la = profile->camera->look_at.value();
		const auto read_f3 = [](u32 at, float out[3]) {
			for (int k = 0; k < 3; k++)
				out[k] = std::bit_cast<float>(memRead32(at + 4u * k));
		};
		const u8 local_slot = cb.has_id_offset ? memRead8(static_cast<u32>(base + cb.id_offset)) : 0;
		const u16 room = memRead16(pp->room);

		// The local poses, into the game's coordinates.
		const VRInputSnapshot snap = GetInputSnapshot();
		Packet out;
		out.slot = local_slot;
		out.room = room;
		read_f3(static_cast<u32>(base + la.position_offset), out.character);
		if (snap.generation != 0 && snap.head_pose.valid &&
			PoseToGame(m, snap.head_pose.position_xyz.data(), snap.head_pose.orientation_xyzw.data(), &out.head))
			out.flags |= kFlagHead;
		for (int h = 0; h < 2; h++)
		{
			const VRHandState& hs = snap.hands[h];
			if (snap.generation == 0 || !snap.actions_active || !hs.grip_pose.valid)
				continue;
			const VRPose& orient = hs.aim_pose.valid ? hs.aim_pose : hs.grip_pose;
			if (PoseToGame(m, hs.grip_pose.position_xyz.data(), orient.orientation_xyzw.data(), &out.hand[h]))
				out.flags |= (h == 0 ? kFlagLeft : kFlagRight);
			out.grip[h] = static_cast<u8>(std::clamp(hs.grip, 0.0f, 1.0f) * 255.0f + 0.5f);
			out.trigger[h] = static_cast<u8>(std::clamp(hs.trigger, 0.0f, 1.0f) * 255.0f + 0.5f);
		}
		const u64 now = NowMs();
		Packet in;
		bool have_in = false;
		u64 in_ms = 0;
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			if (Finite3(out.character))
			{
				s_out = out;
				s_out_valid = true;
				s_out_ms = now;
			}
			if (s_in_valid)
			{
				in = s_in;
				in_ms = s_in_ms;
				have_in = (now >= in_ms && now - in_ms <= kStaleMs);
			}
		}

		// The partner: the record of the packet's slot, used, in this room, as is the packet.
		static int s_partner_state = -1;
		int state = 0;
		PartnerView view; // for the log only (the headset-space poses)
		PartnerGame game;
		u32 found_rec = 0;
		float rec_pos[3] = {};
		if (!have_in)
			state = 1;
		else if (in.slot == local_slot)
			state = 2;
		else if (in.room != room)
			state = 3;
		else
		{
			bool found = false;
			for (u32 k = 0; k < std::max<u32>(cb.count, 1u) && k < 8; k++)
			{
				const s64 rec = static_cast<s64>(cb.indexed_base) + cb.array_offset + static_cast<s64>(k) * cb.stride;
				if (rec < 0 || rec + 0x2000 > static_cast<s64>(Ps2MemSize::MainRam))
					continue;
				const u32 r = static_cast<u32>(rec);
				// Never the local record, whatever its slot byte reads (a stale or duplicate byte mid-change).
				if (r == base)
					continue;
				if (cb.has_id_offset && memRead8(static_cast<u32>(r + cb.id_offset)) != in.slot)
					continue;
				if (!cb.has_id_offset && k != in.slot)
					continue;
				if (memRead16(r + pp->record_used) == 0)
					continue;
				if (memRead16(r + pp->record_room) != room)
				{
					state = 4;
					break;
				}
				read_f3(static_cast<u32>(r + la.position_offset), rec_pos);
				found = Finite3(rec_pos);
				found_rec = r;
				break;
			}
			if (!found && state == 0)
				state = 5;
			if (found)
			{
				const float shift[3] = {rec_pos[0] - in.character[0], rec_pos[1] - in.character[1], rec_pos[2] - in.character[2]};
				const auto near_char = [&in](const GamePose& g) {
					const double dx = g.pos[0] - in.character[0], dy = g.pos[1] - in.character[1], dz = g.pos[2] - in.character[2];
					return std::sqrt(dx * dx + dy * dy + dz * dz) <= kMaxReachCm;
				};
				const auto shifted = [&shift](const GamePose& g) {
					GamePose o = g;
					for (int k = 0; k < 3; k++)
						o.pos[k] += shift[k];
					return o;
				};
				game.record = found_rec;
				game.slot = in.slot;
				game.seq = in.seq;
				if ((in.flags & kFlagHead) && near_char(in.head))
				{
					game.head = shifted(in.head);
					game.flags |= kFlagHead;
				}
				for (int h = 0; h < 2; h++)
				{
					const u8 f = (h == 0) ? kFlagLeft : kFlagRight;
					if ((in.flags & f) && near_char(in.hand[h]))
					{
						game.hand[h] = shifted(in.hand[h]);
						game.flags |= f;
					}
					game.grip[h] = in.grip[h] / 255.0f;
					game.trigger[h] = in.trigger[h] / 255.0f;
				}
				game.valid = game.flags != 0;
				game.stamp_ms = now;
				if ((in.flags & kFlagHead) && near_char(in.head) && PoseFromGame(m, in.head, shift, view.head_pos, view.head_quat))
					view.flags |= kFlagHead;
				for (int h = 0; h < 2; h++)
				{
					const u8 f = (h == 0) ? kFlagLeft : kFlagRight;
					if ((in.flags & f) && near_char(in.hand[h]) &&
						PoseFromGame(m, in.hand[h], shift, view.hand_pos[h], view.hand_quat[h]))
						view.flags |= f;
					view.grip[h] = in.grip[h] / 255.0f;
				}
				view.valid = view.flags != 0;
				view.slot = in.slot;
				view.seq = in.seq;
				view.stamp_ms = now;
				state = view.valid ? 0 : 6;
			}
		}
		{
			std::lock_guard<std::mutex> lock(s_view_mutex);
			s_game = game;
		}
		if (state != s_partner_state)
		{
			s_partner_state = state;
			static const char* const kState[] = {"a partner is placed", "no fresh packet from the peer",
				"the peer's packets carry this console's own slot", "the partner is in another room",
				"the partner's record is in another room", "no used record has the packet's slot",
				"the packet's parts are not usable"};
			Console.WriteLn("(VR) Presence: %s (packet slot %u room %u, this console slot %u room %u).", kState[state],
				have_in ? in.slot : 0u, have_in ? in.room : 0u, local_slot, room);
		}

		// The positions, every 2 s in the loopback test or with PCSX2_VR_PRESENCE_LOG=1.
		static u64 s_last_log = 0;
		if ((loopback_test || s_log_env) && now - s_last_log >= 2000)
		{
			s_last_log = now;
			const auto f3 = [](const float v[3]) { return fmt::format("({:.3f}, {:.3f}, {:.3f})", v[0], v[1], v[2]); };
			const float* hp = snap.head_pose.position_xyz.data();
			Console.WriteLn("(VR) Presence: local head VR %s m, slot %u at game %s; sent %u, received %u (dropped %u foreign, %u "
							"bad, %u wrong key, %u stale).",
				f3(hp).c_str(), local_slot, f3(out.character).c_str(), s_sent.load(), s_received.load(), s_foreign.load(),
				s_bad.load(), s_wrong_key.load(), s_stale.load());
			if (view.valid)
			{
				float hf[3] = {0.0f, 0.0f, 0.0f};
				Store(Rotate(view.head_quat, V3{0.0, 0.0, -1.0}), hf);
				const std::string head = (view.flags & kFlagHead) ? f3(view.head_pos) : std::string("-");
				const std::string left = (view.flags & kFlagLeft) ? f3(view.hand_pos[0]) : std::string("-");
				const std::string right = (view.flags & kFlagRight) ? f3(view.hand_pos[1]) : std::string("-");
				Console.WriteLn("(VR) Presence: partner slot %u VR head %s facing %s, left %s, right %s m; its record at game "
								"%s, the packet's character %s, seq %u, %llu ms old.",
					view.slot, head.c_str(), f3(hf).c_str(), left.c_str(), right.c_str(), f3(rec_pos).c_str(),
					f3(in.character).c_str(), view.seq, static_cast<unsigned long long>(now - in_ms));
			}
		}
	}

	// --- Self-test -----------------------------------------------------------------------------------------------------
	bool SelfTest()
	{
		int checks = 0, failed = 0;
		const auto check = [&](bool ok, const char* name) {
			checks++;
			if (!ok)
			{
				failed++;
				Console.Error("(VR) Presence self-test FAIL: %s", name);
			}
		};
		const auto near3 = [](const float a[3], const float b[3], double tol) {
			return Len(Load(a) - Load(b)) <= tol;
		};

		// --- Encode / decode ---
		{
			Packet p;
			p.slot = 2;
			p.flags = kFlagHead | kFlagRight;
			p.room = 0x1234;
			p.key = KeyHash("brothers");
			p.session = 0xDEADBEEF;
			p.seq = 77;
			p.send_ms = 123456;
			p.character[0] = 8497.5f;
			p.character[1] = -12.25f;
			p.character[2] = -3021.0f;
			p.head.pos[0] = 8500.0f;
			p.head.pos[1] = 143.0f;
			p.head.pos[2] = -3000.0f;
			p.hand[1].pos[0] = 8520.0f;
			p.hand[1].fwd[0] = 1.0f;
			p.hand[1].fwd[2] = 0.0f;
			p.grip[1] = 200;
			p.trigger[0] = 9;
			u8 buf[kPacketSize];
			check(Encode(p, buf, sizeof(buf)), "encode fills the 156 bytes");
			check(!Encode(p, buf, kPacketSize - 1), "encode refuses a short buffer");
			Packet q;
			check(Decode(buf, sizeof(buf), &q) && q.slot == 2 && q.flags == p.flags && q.room == 0x1234 && q.key == p.key &&
					  q.session == p.session && q.seq == 77 && q.send_ms == 123456 && q.character[0] == 8497.5f &&
					  q.character[1] == -12.25f && q.head.pos[1] == 143.0f && q.hand[1].fwd[0] == 1.0f && q.grip[1] == 200 &&
					  q.trigger[0] == 9,
				"decode gives back every field exactly");
			check(!Decode(buf, sizeof(buf) - 1, &q) && !Decode(buf, sizeof(buf) + 0, nullptr), "a wrong size or no output: refused");
			u8 bad[kPacketSize];
			std::memcpy(bad, buf, sizeof(buf));
			bad[50] ^= 0x01;
			check(!Decode(bad, sizeof(bad), &q), "one flipped bit: the checksum refuses it");
			std::memcpy(bad, buf, sizeof(buf));
			bad[0] = 'X';
			check(!Decode(bad, sizeof(bad), &q), "another magic: refused");
			Packet s = p;
			s.slot = 4;
			Encode(s, bad, sizeof(bad));
			check(!Decode(bad, sizeof(bad), &q), "a slot past 3: refused");
			s = p;
			s.head.fwd[0] = 3.0f;
			Encode(s, bad, sizeof(bad));
			check(!Decode(bad, sizeof(bad), &q), "a valid head whose forward is not unit: refused");
			s = p;
			s.flags = 0;
			s.head.fwd[0] = 3.0f;
			Encode(s, bad, sizeof(bad));
			check(Decode(bad, sizeof(bad), &q), "the same head marked not valid: taken (its numbers are not used)");
			s = p;
			s.character[2] = std::numeric_limits<float>::quiet_NaN();
			Encode(s, bad, sizeof(bad));
			check(!Decode(bad, sizeof(bad), &q), "a character position that is not a number: refused");
			s = p;
			s.flags = 0x08;
			Encode(s, bad, sizeof(bad));
			check(!Decode(bad, sizeof(bad), &q), "an unknown flag: refused");
			check(KeyHash("brothers") != KeyHash("Brothers") && KeyHash("") == 2166136261u, "the key hash tells words apart");
			check(!KeyUsable("") && !KeyUsable("  ") && KeyUsable("brothers"), "an empty key (a fixed public hash) is refused");
		}

		// --- The private-address filter and the peer parser ---
		{
			const auto ip = [](const char* t) {
				u32 a = 0;
				return ParseIPv4(t, &a) ? a : 0u;
			};
			check(ip("192.168.1.20") == 0xC0A80114u && ip("100.101.102.103") == 0x64656667u, "dotted quads parse");
			check(ip("256.1.1.1") == 0 && ip("1.2.3") == 0 && ip("1.2.3.4.5") == 0 && ip("1.2.3.4 ") == 0 && ip("a.b.c.d") == 0 &&
					  ip("01234.1.1.1") == 0,
				"malformed quads do not");
			check(IsPrivateIPv4(ip("10.0.0.5")) && IsPrivateIPv4(ip("172.16.0.1")) && IsPrivateIPv4(ip("172.31.255.255")) &&
					  IsPrivateIPv4(ip("192.168.0.9")) && IsPrivateIPv4(ip("100.64.0.1")) && IsPrivateIPv4(ip("100.127.255.254")) &&
					  IsPrivateIPv4(ip("127.0.0.1")),
				"LAN, Tailscale and loopback are private");
			check(!IsPrivateIPv4(ip("172.32.0.1")) && !IsPrivateIPv4(ip("172.15.0.1")) && !IsPrivateIPv4(ip("100.63.255.255")) &&
					  !IsPrivateIPv4(ip("100.128.0.1")) && !IsPrivateIPv4(ip("8.8.8.8")) && !IsPrivateIPv4(ip("192.169.0.1")) &&
					  !IsPrivateIPv4(ip("169.254.1.1")) && !IsPrivateIPv4(0) && !IsPrivateIPv4(ip("255.255.255.255")),
				"the edges of each range and the public internet are not");
			check(IsLoopbackIPv4(ip("127.0.0.1")) && IsLoopbackIPv4(ip("127.5.5.5")) && !IsLoopbackIPv4(ip("10.0.0.1")),
				"loopback is 127/8");
			std::string h;
			u16 port = 0;
			check(ParsePeer("100.88.1.2:47800", &h, &port) && h == "100.88.1.2" && port == 47800, "host:port parses");
			check(ParsePeer(" brother-pc.tailnet-x.ts.net:5000 ", &h, &port) && h == "brother-pc.tailnet-x.ts.net" && port == 5000,
				"a name with spaces round it parses");
			check(!ParsePeer("100.88.1.2", &h, &port) && !ParsePeer(":47800", &h, &port) && !ParsePeer("a:0", &h, &port) &&
					  !ParsePeer("a:65536", &h, &port) && !ParsePeer("a:12x", &h, &port) && !ParsePeer("a b:1", &h, &port) &&
					  !ParsePeer("[::1]:5", &h, &port) && !ParsePeer("a:", &h, &port) && !ParsePeer("", &h, &port),
				"no port, no host, a port out of range, junk or IPv6: refused");
		}

		// --- The stale and reorder filter ---
		{
			SeqState st;
			check(Accept(st, 5, 100, 1000), "the first packet is taken");
			check(Accept(st, 5, 101, 1010) && Accept(st, 5, 105, 1020), "later numbers are taken (gaps are fine)");
			check(!Accept(st, 5, 105, 1030) && !Accept(st, 5, 103, 1031), "a repeat or an older one is dropped");
			check(!Accept(st, 9, 1, 1500), "another session while this one is live: dropped (a late packet of an old run)");
			check(st.seq == 105 && st.session == 5 && st.last_ms == 1020, "a drop changes nothing");
			check(Accept(st, 9, 1, 2100), "another session once this one has been quiet a second: taken (a restart)");
			check(!Accept(st, 5, 106, 2110), "the old session after that: dropped");
			SeqState w;
			Accept(w, 1, 0xFFFFFFF0u, 0);
			check(Accept(w, 1, 5, 10) && !Accept(w, 1, 0xFFFFFFF8u, 20), "the sequence number wraps");
		}

		// --- The pose mapping round trip ---
		{
			CameraDriver::Mapping a;
			a.valid = true;
			a.character[0] = 8497.0f;
			a.character[1] = 0.0f;
			a.character[2] = -3021.0f;
			a.eye_height = 155.0f;
			a.base_yaw = 1.1f;
			a.ref_pos[0] = 0.05f;
			a.ref_pos[1] = 1.15f;
			a.ref_pos[2] = -0.02f;
			a.ref_yaw = 0.3f;
			a.units_per_meter = 100.0f;
			a.yaw_sign = 1.0f;
			CameraDriver::Mapping mirror = a;
			mirror.yaw_sign = -1.0f;
			mirror.base_yaw = -2.0f;
			const float pos[3] = {0.2f, 1.3f, -0.4f};
			const float half = 0.35f;
			const float qraw[4] = {0.1f, std::sin(half), 0.05f, std::cos(half)};
			float q[4];
			UnitQuat(qraw, q);
			for (const CameraDriver::Mapping* m : {&a, &mirror})
			{
				GamePose g;
				float back_pos[3], back_q[4];
				const bool ok = PoseToGame(*m, pos, q, &g) && PoseFromGame(*m, g, nullptr, back_pos, back_q);
				const V3 f0 = Rotate(q, V3{0, 0, -1}), f1 = Rotate(back_q, V3{0, 0, -1});
				const V3 u0 = Rotate(q, V3{0, 1, 0}), u1 = Rotate(back_q, V3{0, 1, 0});
				check(ok && near3(pos, back_pos, 1.0e-4) && Len(f0 - f1) < 1.0e-3 && Len(u0 - u1) < 1.0e-3,
					m == &a ? "VR to game and back: the same pose (within 0.1 mm, 0.06 deg)"
							: "the same through a mirroring map (yaw_sign -1)");
				check(ok && std::fabs(Len(Load(g.fwd)) - 1.0) < 1.0e-4 && std::fabs(Dot(Load(g.fwd), Load(g.up))) < 1.0e-3,
					"the game pose's directions are unit and perpendicular");
			}
			// A head 1 m in front of the recentred head along the headset's forward, mapped, is 100 game units from the eye.
			{
				const float ahead[3] = {a.ref_pos[0] - std::sin(a.ref_yaw), a.ref_pos[1], a.ref_pos[2] - std::cos(a.ref_yaw)};
				const float ident[4] = {0, 0, 0, 1};
				GamePose g;
				const bool ok = PoseToGame(a, ahead, ident, &g);
				const double dx = g.pos[0] - a.character[0], dz = g.pos[2] - a.character[2];
				check(ok && std::fabs(std::sqrt(dx * dx + dz * dz) - 100.0) < 0.01 && std::fabs(g.pos[1] - 155.0f) < 0.01f,
					"a point 1 m ahead lands 100 game units out at eye height");
			}
			// The partner: the same game pose seen through another console's mapping, moved by its copy of the character.
			{
				CameraDriver::Mapping b = a;
				b.character[0] = 9100.0f;
				b.character[2] = -2800.0f;
				b.base_yaw = -0.4f;
				b.ref_yaw = 1.0f;
				b.ref_pos[0] = -0.3f;
				GamePose g;
				PoseToGame(a, pos, q, &g);
				const float shift[3] = {50.0f, 0.0f, -20.0f};
				float pb[3], qb[4], gb[3];
				const bool ok = PoseFromGame(b, g, shift, pb, qb) && CameraDriver::GameFromVr(b, pb, gb);
				const float want[3] = {g.pos[0] + 50.0f, g.pos[1], g.pos[2] - 20.0f};
				check(ok && near3(gb, want, 0.01), "the receiver's VR point maps back to the shifted game point");
				GamePose g2;
				check(ok && PoseToGame(b, pb, qb, &g2) && Len(Load(g2.fwd) - Load(g.fwd)) < 1.0e-3 &&
						  Len(Load(g2.up) - Load(g.up)) < 1.0e-3,
					"and faces the same way in the game");
			}
			CameraDriver::Mapping bad = a;
			bad.valid = false;
			GamePose g;
			float bp[3], bq[4];
			const float nanp[3] = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
			const float zq[4] = {0, 0, 0, 0};
			check(!PoseToGame(bad, pos, q, &g) && !PoseToGame(a, nanp, q, &g) && !PoseToGame(a, pos, zq, &g) &&
					  !PoseFromGame(bad, g, nullptr, bp, bq),
				"an unusable mapping, position or orientation: refused");
		}

		if (failed == 0)
			Console.WriteLn("(VR) Presence self-test: %d checks passed.", checks);
		else
			Console.Error("(VR) Presence self-test: %d of %d checks FAILED.", failed, checks);
		return failed == 0;
	}
} // namespace VR::Presence
