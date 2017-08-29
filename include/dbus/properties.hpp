#include <dbus/connection.hpp>
#include <dbus/filter.hpp>
#include <dbus/match.hpp>
#include <functional>
#include <tuple>
#include <type_traits>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>

namespace dbus {
struct DbusArgument {
  DbusArgument(const std::string& direction, const std::string& name,
               const std::string& type)
      : direction(direction), name(name), type(type){};
  std::string direction;
  std::string name;
  std::string type;
};

class DbusMethod {
 public:
  DbusMethod(std::string name, std::shared_ptr<dbus::connection>& conn)
      : name(name), conn(conn){};
  virtual void call(dbus::message& m){};
  virtual std::vector<DbusArgument> get_args() { return {}; };
  std::string name;
  std::shared_ptr<dbus::connection> conn;
};

enum class UpdateType { VALUE_CHANGE_ONLY, FORCE };

// primary template.
template <class T>
struct function_traits : function_traits<decltype(&T::operator())> {};

// partial specialization for function type
template <class R, class... Args>
struct function_traits<R(Args...)> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
  using decayed_arg_types = std::tuple<typename std::decay<Args>::type...>;
};

// partial specialization for function pointer
template <class R, class... Args>
struct function_traits<R (*)(Args...)> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
  using decayed_arg_types = std::tuple<typename std::decay<Args>::type...>;
};

// partial specialization for std::function
template <class R, class... Args>
struct function_traits<std::function<R(Args...)>> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
  using decayed_arg_types = std::tuple<typename std::decay<Args>::type...>;
};

// partial specialization for pointer-to-member-function (i.e., operator()'s)
template <class T, class R, class... Args>
struct function_traits<R (T::*)(Args...)> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
  using decayed_arg_types = std::tuple<typename std::decay<Args>::type...>;
};

template <class T, class R, class... Args>
struct function_traits<R (T::*)(Args...) const> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
  using decayed_arg_types = std::tuple<typename std::decay<Args>::type...>;
};

template <class F, size_t... Is>
constexpr auto index_apply_impl(F f, std::index_sequence<Is...>) {
  return f(std::integral_constant<size_t, Is>{}...);
}

template <size_t N, class F>
constexpr auto index_apply(F f) {
  return index_apply_impl(f, std::make_index_sequence<N>{});
}

template <class Tuple, class F>
constexpr auto apply(Tuple& t, F f) {
  return index_apply<std::tuple_size<Tuple>{}>(
      [&](auto... Is) { return f(std::get<Is>(t)...); });
}

template <class Tuple>
constexpr void unpack_into_tuple(Tuple& t, dbus::message& m) {
  return index_apply<std::tuple_size<Tuple>{}>(
      [&](auto... Is) { m.unpack(std::get<Is>(t)...); });
}

// Specialization for empty tuples.  No need to unpack if no arguments
constexpr void unpack_into_tuple(std::tuple<>& t, dbus::message& m) {}

template <class Tuple>
constexpr void pack_tuple_into_msg(Tuple& t, dbus::message& m) {
  return index_apply<std::tuple_size<Tuple>{}>(
      [&](auto... Is) { m.pack(std::get<Is>(t)...); });
}

// Specialization for empty tuples.  No need to pack if no arguments
constexpr void pack_tuple_into_msg(std::tuple<>& t, dbus::message& m) {}

// Base case for when I == the size of the tuple args.  Does nothing, as we
// should be done
template <std::size_t I = 0, typename... Tp>
inline typename std::enable_if<I == sizeof...(Tp), void>::type arg_types(
    bool in, std::tuple<Tp...>& t, std::vector<DbusArgument>& v) {}

// Case for when I < the size of tuple args.  Unpacks the tuple type into the
// dbusargument object and names it appropriately.
template <std::size_t I = 0, typename... Tp>
    inline typename std::enable_if <
    I<sizeof...(Tp), void>::type arg_types(bool in, std::tuple<Tp...>& t,
                                           std::vector<DbusArgument>& v) {
  typedef typename std::tuple_element<I, std::tuple<Tp...>>::type element_type;
  auto constexpr sig = element_signature<element_type>::code;

  if (in) {
    auto name = "arg_" + std::to_string(I);
    v.emplace_back("in", name, &sig[0]);
  } else {
    auto name = "out_" + std::to_string(I);
    v.emplace_back("out", name, &sig[0]);
  }
  arg_types<I + 1, Tp...>(in, t, v);
}

template <typename Handler>
class LambdaDbusMethod : public DbusMethod {
 public:
  typedef function_traits<Handler> traits;
  typedef typename traits::decayed_arg_types InputTupleType;
  typedef typename traits::result_type ResultTupleType;
  LambdaDbusMethod(std::string name, std::shared_ptr<dbus::connection>& conn,
                   Handler h)
      : DbusMethod(name, conn), h(std::move(h)) {}
  void call(dbus::message& m) override {
    InputTupleType input_args;
    unpack_into_tuple(input_args, m);
    ResultTupleType r = apply(input_args, h);
    auto ret = dbus::message::new_return(m);
    pack_tuple_into_msg(r, ret);
    conn->send(ret, std::chrono::seconds(0));
  };

  std::vector<DbusArgument> get_args() override {
    std::vector<DbusArgument> ret;
    std::string s;
    InputTupleType t;
    arg_types(true, t, ret);

    ResultTupleType o;
    arg_types(false, o, ret);
    return ret;
  };
  Handler h;
};

class DbusSignal {
 public:
  DbusSignal(std::string name) : name(name){};
  std::string name;

  virtual std::vector<DbusArgument> get_args() { return {}; };
};

class DbusInterface {
 public:
  DbusInterface(std::string interface_name,
                std::shared_ptr<dbus::connection>& conn)
      : interface_name(std::move(interface_name)), conn(conn) {}
  virtual boost::container::flat_map<std::string, std::shared_ptr<DbusSignal>>
  get_signals() {
    return dbus_signals;
  };
  virtual boost::container::flat_map<std::string, std::shared_ptr<DbusMethod>>
  get_methods() {
    return dbus_methods;
  };
  virtual std::string get_interface_name() { return interface_name; };
  virtual const boost::container::flat_map<std::string, dbus_variant>
  get_properties_map() {
    return properties_map;
  };

  template <typename VALUE_TYPE>
  void set_property(const std::string& property_name, const VALUE_TYPE value,
                    UpdateType update_mode = UpdateType::VALUE_CHANGE_ONLY) {
    // Construct a change vector of length 1.  if this set_properties is ever
    // templated for any type, we could probably swap with with a
    // std::array<pair, 1>
    std::vector<std::pair<std::string, dbus_variant>> v;
    v.emplace_back(property_name, value);
    set_properties(v, update_mode);
  }

  void set_properties(
      const std::vector<std::pair<std::string, dbus_variant>>& v,
      const UpdateType update_mode = UpdateType::VALUE_CHANGE_ONLY) {
    // TODO(ed) generalize this interface for all "map like" types, basically
    // anything that will return a const iterator of std::pair<string,
    // variant>
    std::vector<std::pair<std::string, dbus_variant>> updates;
    updates.reserve(v.size());

    if (update_mode == UpdateType::FORCE) {
      updates = v;
    } else {
      for (auto& property : v) {
        auto property_map_it = properties_map.find(property.first);
        if (property_map_it != properties_map.end()) {
          // Property exists in map
          if (property_map_it->second != property.second) {
            properties_map[property.first] = property.second;
            // if value has changed since last set
            updates.emplace_back(*property_map_it);
          }
        } else {
          // property doesn't exist, must be new
          properties_map[property.first] = property.second;
          updates.emplace_back(property.first, property.second);
        }
      }
    }

    const static dbus::endpoint endpoint("org.freedesktop.DBus", object_name,
                                         "org.freedesktop.DBus.Properties");

    auto m = dbus::message::new_signal(endpoint, "PropertiesChanged");

    static const std::vector<std::string> empty;
    m.pack(get_interface_name(), updates, empty);
    // TODO(ed) make sure this doesn't block
    conn->async_send(
        m, [](const boost::system::error_code ec, dbus::message r) {});
  }

  void register_method(std::shared_ptr<DbusMethod>&& method) {
    dbus_methods.emplace(method->name, method);
  }

  template <typename Handler>
  void register_method(const std::string& name, Handler&& method) {
    dbus_methods.emplace(name,
                         new LambdaDbusMethod<Handler>(name, conn, method));
  }

  void call(dbus::message& m) {
    std::string method_name = m.get_member();
    auto method = dbus_methods.find(method_name);
    if (method != dbus_methods.end()) {
      method->second->call(m);
    }  // TODO(ed) send something when method doesn't exist?
  }

  std::string object_name;
  std::string interface_name;
  boost::container::flat_map<std::string, std::shared_ptr<DbusMethod>>
      dbus_methods;
  boost::container::flat_map<std::string, std::shared_ptr<DbusSignal>>
      dbus_signals;
  boost::container::flat_map<std::string, dbus_variant> properties_map;
  std::shared_ptr<dbus::connection> conn;
};

class DbusObject {
 public:
  DbusObject(std::shared_ptr<dbus::connection> conn, std::string object_name)
      : object_name(std::move(object_name)), conn(conn) {
    properties_iface = add_interface("org.freedesktop.DBus.Properties");

    properties_iface->register_method(
        "Get", [&](const std::string& interface_name,
                   const std::string& property_name) {
          auto interface_it = interfaces.find(interface_name);
          if (interface_it == interfaces.end()) {
            // Interface not found error
            throw std::runtime_error("interface not found");
          } else {
            auto& properties_map = interface_it->second->get_properties_map();
            auto property = properties_map.find(property_name);
            if (property == properties_map.end()) {
              // TODO(ed) property not found error
              throw std::runtime_error("property not found");
            } else {
              return std::tuple<dbus_variant>(property->second);
            }
          }
        });

    properties_iface->register_method("GetAll", [&](const std::string&
                                                        interface_name) {
      auto interface_it = interfaces.find(interface_name);
      if (interface_it == interfaces.end()) {
        // Interface not found error
        throw std::runtime_error("interface not found");
      } else {
        std::vector<std::pair<std::string, dbus_variant>> v;
        for (auto& element : properties_iface->get_properties_map()) {
          v.emplace_back(element.first, element.second);
        }
        return std::tuple<std::vector<std::pair<std::string, dbus_variant>>>(v);
      }
    });
    // TODO(Ed) support returning nothing
    properties_iface->register_method(
        "Set",
        [&](const std::string& interface_name, const std::string& property_name,
            const dbus_variant& value) {
          auto interface_it = interfaces.find(interface_name);
          if (interface_it == interfaces.end()) {
            // Interface not found error
            throw std::runtime_error("interface not found");
          } else {
            // Todo, the set propery (signular) interface should support
            // handing
            // a variant.  THe below is expensive
            std::vector<std::pair<std::string, dbus_variant>> v;
            v.emplace_back(property_name, value);
            interface_it->second->set_properties(v);
            return std::tuple<>();
          }
        });
  }

  std::shared_ptr<DbusInterface> add_interface(const std::string& name) {
    auto x = std::make_shared<DbusInterface>(name, conn);
    register_interface(x);
    return x;
  }

  void register_interface(std::shared_ptr<DbusInterface>& interface) {
    interfaces[interface->get_interface_name()] = interface;
    interface->object_name = object_name;
    const static dbus::endpoint endpoint("", object_name,
                                         "org.freedesktop.DBus.ObjectManager");

    const static std::string signal_name("InterfacesAdded");
    auto m = message::new_signal(endpoint, signal_name);
    typedef std::vector<std::pair<std::string, dbus_variant>> properties_dict;
    std::vector<std::pair<std::string, properties_dict>> sig;
    sig.emplace_back(interface->get_interface_name(), properties_dict());
    auto& prop_dict = sig.back().second;
    for (auto& property : interface->get_properties_map()) {
      prop_dict.emplace_back(property);
    }

    m.pack(object_name, sig);
    conn->send(m);
  }

  auto get_interfaces() { return interfaces; }

  void call(dbus::message& m) {
    auto interface = interfaces.find(m.get_interface());
    if (interface != interfaces.end()) {
      interface->second->call(m);
    }  // TODO(ed) send something when interface doesn't exist?
  }

  std::string object_name;
  std::shared_ptr<dbus::connection> conn;

  // dbus::filter properties_filter;
  std::shared_ptr<DbusInterface> properties_iface;

  std::function<void(boost::system::error_code, message)> callback;
  boost::container::flat_map<std::string, std::shared_ptr<DbusInterface>>
      interfaces;
};

class DbusObjectServer {
 public:
  DbusObjectServer(std::shared_ptr<dbus::connection>& conn) : conn(conn) {
    introspect_filter =
        std::make_unique<dbus::filter>(conn, [](dbus::message m) {
          if (m.get_type() != "method_call") {
            return false;
          }
          if (m.get_interface() != "org.freedesktop.DBus.Introspectable") {
            return false;
          }
          if (m.get_member() != "Introspect") {
            return false;
          };
          return true;
        });

    introspect_filter->async_dispatch(
        [&](const boost::system::error_code ec, dbus::message m) {
          on_introspect(ec, m);
        });

    object_manager_filter =
        std::make_unique<dbus::filter>(conn, [](dbus::message m) {

          if (m.get_type() != "method_call") {
            return false;
          }
          if (m.get_interface() != "org.freedesktop.DBus.ObjectManager") {
            return false;
          }
          if (m.get_member() != "GetManagedObjects") {
            return false;
          };
          return true;
        });

    object_manager_filter->async_dispatch(
        [&](const boost::system::error_code ec, dbus::message m) {
          on_get_managed_objects(ec, m);
        });

    method_filter = std::make_unique<dbus::filter>(conn, [](dbus::message m) {

      if (m.get_type() != "method_call") {
        return false;
      }
      return true;
    });

    method_filter->async_dispatch(
        [&](const boost::system::error_code ec, dbus::message m) {
          on_method_call(ec, m);
        });
  };

  std::shared_ptr<dbus::connection>& get_connection() { return conn; }
  void on_introspect(const boost::system::error_code ec, dbus::message m) {
    auto xml = get_xml_for_path(m.get_path());
    auto ret = dbus::message::new_return(m);
    ret.pack(xml);
    conn->async_send(
        ret, [](const boost::system::error_code ec, dbus::message r) {});

    introspect_filter->async_dispatch(
        [&](const boost::system::error_code ec, dbus::message m) {
          on_introspect(ec, m);
        });
  }

  void on_method_call(const boost::system::error_code ec, dbus::message m) {
    std::cout << "on method call\n";
    if (ec) {
      std::cerr << "on_method_call error: " << ec << "\n";
    } else {
      auto path = m.get_path();
      // TODO(ed) objects should be a map
      for (auto& object : objects) {
        if (object->object_name == path) {
          object->call(m);
          break;
        }
      }
    }
    method_filter->async_dispatch(
        [&](const boost::system::error_code ec, dbus::message m) {
          on_method_call(ec, m);
        });
  }

  void on_get_managed_objects(const boost::system::error_code ec,
                              dbus::message m) {
    typedef std::vector<std::pair<std::string, dbus::dbus_variant>>
        properties_dict;

    typedef std::vector<std::pair<std::string, properties_dict>>
        interfaces_dict;

    std::vector<std::pair<std::string, interfaces_dict>> dict;

    for (auto& object : objects) {
      interfaces_dict i;
      for (auto& interface : object->get_interfaces()) {
        properties_dict p;

        for (auto& property : interface.second->get_properties_map()) {
          p.push_back(property);
        }

        i.emplace_back(interface.second->get_interface_name(), std::move(p));
      }
      dict.emplace_back(object->object_name, std::move(i));
    }
    auto ret = dbus::message::new_return(m);
    ret.pack(dict);
    conn->async_send(
        ret, [](const boost::system::error_code ec, dbus::message r) {});

    object_manager_filter->async_dispatch(
        [&](const boost::system::error_code ec, dbus::message m) {
          on_get_managed_objects(ec, m);
        });
  }

  std::shared_ptr<DbusObject> add_object(const std::string& name) {
    auto x = std::make_shared<DbusObject>(conn, name);
    register_object(x);
    return x;
  }

  void register_object(std::shared_ptr<DbusObject> object) {
    objects.emplace_back(object);
  }

  std::string get_xml_for_path(const std::string& path) {
    std::string newpath(path);

    if (newpath == "/") {
      newpath.assign("");
    }

    boost::container::flat_set<std::string> node_names;
    std::string xml(
        "<!DOCTYPE node PUBLIC "
        "\"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
        "\"http://www.freedesktop.org/standards/dbus/1.0/"
        "introspect.dtd\">\n<node>");
    for (auto& object : objects) {
      std::string& object_name = object->object_name;
      // exact match
      if (object->object_name == newpath) {
        xml +=
            "  <interface name=\"org.freedesktop.DBus.Peer\">"
            "    <method name=\"Ping\"/>"
            "    <method name=\"GetMachineId\">"
            "      <arg type=\"s\" name=\"machine_uuid\" direction=\"out\"/>"
            "    </method>"
            "  </interface>";

        xml +=
            "  <interface name=\"org.freedesktop.DBus.ObjectManager\">"
            "    <method name=\"GetManagedObjects\">"
            "      <arg type=\"a{oa{sa{sv}}}\" "
            "           name=\"object_paths_interfaces_and_properties\" "
            "           direction=\"out\"/>"
            "    </method>"
            "    <signal name=\"InterfacesAdded\">"
            "      <arg type=\"o\" name=\"object_path\"/>"
            "      <arg type=\"a{sa{sv}}\" "
            "name=\"interfaces_and_properties\"/>"
            "    </signal>"
            "    <signal name=\"InterfacesRemoved\">"
            "      <arg type=\"o\" name=\"object_path\"/>"
            "      <arg type=\"as\" name=\"interfaces\"/>"
            "    </signal>"
            "  </interface>";

        for (auto& interface_pair : object->interfaces) {
          xml += "<interface name=\"";
          xml += interface_pair.first;
          xml += "\">";
          for (auto& method : interface_pair.second->get_methods()) {
            xml += "<method name=\"";
            xml += method.first;
            xml += "\">";
            for (auto& arg : method.second->get_args()) {
              xml += "<arg name=\"";
              xml += arg.name;
              xml += "\" type=\"";
              xml += arg.type;
              xml += "\" direction=\"";
              xml += arg.direction;
              xml += "\"/>";
            }
            xml += "</method>";
          }

          for (auto& signal : interface_pair.second->get_signals()) {
            xml += "<signal name=\"";
            xml += signal.first;
            xml += "\">";
            for (auto& arg : signal.second->get_args()) {
              xml += "<arg name=\"";
              xml += arg.name;
              xml += "\" type=\"";
              xml += arg.type;
              xml += "\"/>";
            }

            xml += "</signal>";
          }

          for (auto& property : interface_pair.second->get_properties_map()) {
            xml += "<property name=\"";
            xml += property.first;
            xml += "\" type =\"";

            std::string type = std::string(boost::apply_visitor(
                [&](auto val) {
                  static const auto constexpr sig =
                      element_signature<decltype(val)>::code;
                  return &sig[0];
                },
                property.second));
            xml += type;
            xml += "\" direction =\"";
            // TODO direction can be readwrite, read, or write.  Need to
            // make this configurable
            xml += "readwrite";
            xml += "\"/>";
          }
          xml += "</interface>";
        }
      } else if (boost::starts_with(object_name, newpath)) {
        auto slash_index = object_name.find("/", newpath.size() + 1);
        auto subnode = object_name.substr(newpath.size() + 1,
                                          slash_index - newpath.size() - 1);
        if (node_names.find(subnode) == node_names.end()) {
          node_names.insert(subnode);
          xml += "<node name=\"";
          xml += subnode;
          xml += "\">";
          xml += "</node>";
        }
      }
    }
    xml += "</node>";
    return xml;
  }

 private:
  std::shared_ptr<dbus::connection> conn;
  std::vector<std::shared_ptr<DbusObject>> objects;
  std::unique_ptr<dbus::filter> introspect_filter;
  std::unique_ptr<dbus::filter> object_manager_filter;
  std::unique_ptr<dbus::filter> method_filter;
};
}