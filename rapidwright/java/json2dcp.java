package dev.fpga.rapidwright;

import com.google.gson.stream.JsonReader;
import com.xilinx.rapidwright.design.*;
import com.xilinx.rapidwright.device.PartNameTools;
import com.xilinx.rapidwright.device.*;
import com.google.gson.*;
import com.xilinx.rapidwright.util.RapidWright;
import jnr.ffi.annotations.In;
import org.json.JSONObject;

import java.io.FileNotFoundException;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

public class json2dcp {

    static class NextpnrNet {

        public NextpnrNet(String name) {
            this.name = name;
            this.params = new HashMap<>();
            this.attrs = new HashMap<>();
            this.driver = null;
            this.users = new ArrayList<>();
            this.rwNet = null;
        }

        public String name;
        public HashMap<String, String> params;
        public HashMap<String, String> attrs;
        public NextpnrCellPort driver;
        public ArrayList<NextpnrCellPort> users;
        public Net rwNet;
    }

    enum PortDirection {
        PORT_IN,
        PORT_OUT,
        PORT_INOUT
    }

    public static PortDirection parsePortDir(String s) {
        if (s.equals("input"))
            return PortDirection.PORT_IN;
        else if (s.equals("output"))
            return PortDirection.PORT_OUT;
        else if (s.equals("inout"))
            return PortDirection.PORT_INOUT;
        else
            throw new RuntimeException("bad port direction " + s);
    }

    static class NextpnrCellPort {

        public NextpnrCellPort(NextpnrCell cell, String name, PortDirection type) {
            this.cell = cell;
            this.name = name;
            this.type = type;
            this.net = null;
        }

        public NextpnrCell cell;
        public String name;
        public PortDirection type;
        public NextpnrNet net;
    }

    static class NextpnrCell {

        public NextpnrCell(String name, String type) {
            this.name = name;
            this.type = type;
            this.ports = new HashMap<>();
            this.params = new HashMap<>();
            this.attrs = new HashMap<>();
            this.rwCell = null;
        }

        public String name, type;
        public HashMap<String, NextpnrCellPort> ports;
        public HashMap<String, String> params;
        public HashMap<String, String> attrs;
        public Cell rwCell;
    }

    static class NextpnrDesign {
        // json index --> net
        public HashMap<Integer, NextpnrNet> nets;
        // name --> cell
        public HashMap<String, NextpnrCell> cells;
        public Design rwd;

        public NextpnrDesign() {
            nets = new HashMap<>();
            cells = new HashMap<>();
            rwd = null;
        }

        void Import(JsonObject des) {
            JsonObject top = des.getAsJsonObject("modules").getAsJsonObject(des.getAsJsonObject("modules").keySet().toArray()[0].toString());
            JsonObject netJson = top.getAsJsonObject("netnames");
            for(Map.Entry<String, JsonElement> entry : netJson.entrySet()) {
                NextpnrNet net = new NextpnrNet(entry.getKey());
                JsonObject data = entry.getValue().getAsJsonObject();
                int index = data.getAsJsonArray("bits").get(0).getAsInt();

                if (data.has("attributes")) {
                    JsonObject attrs = data.getAsJsonObject("attributes");
                    for (Map.Entry<String, JsonElement> attr : attrs.entrySet())
                        net.attrs.put(attr.getKey(), attr.getValue().getAsString());
                }

                nets.put(index, net);
            }

            JsonObject cellJson = top.getAsJsonObject("cells");
            for(Map.Entry<String, JsonElement> entry : cellJson.entrySet()) {
                JsonObject data = entry.getValue().getAsJsonObject();
                NextpnrCell cell = new NextpnrCell(entry.getKey(), data.get("type").getAsString());

                JsonObject pdirs = data.getAsJsonObject("port_directions");
                for (Map.Entry<String, JsonElement> pdir : pdirs.entrySet())
                    cell.ports.put(pdir.getKey(), new NextpnrCellPort(cell, pdir.getKey(), parsePortDir(pdir.getValue().getAsString())));

                JsonObject pconns = data.getAsJsonObject("connections");
                for (Map.Entry<String, JsonElement> pc : pconns.entrySet()) {
                    NextpnrCellPort port = cell.ports.get(pc.getKey());
                    JsonArray conn = pc.getValue().getAsJsonArray();
                    if (conn.size() > 0) {
                        NextpnrNet net = nets.get(conn.get(0).getAsInt());
                        port.net = net;
                        if (port.type == PortDirection.PORT_OUT) {
                            net.driver = port;
                        } else {
                            net.users.add(port);
                        }
                    }

                }

                JsonObject attrs = data.getAsJsonObject("attributes");
                for (Map.Entry<String, JsonElement> attr : attrs.entrySet())
                    cell.attrs.put(attr.getKey(), attr.getValue().getAsString());

                // FIXME: parse numerical params correctly
                JsonObject params = data.getAsJsonObject("parameters");
                for (Map.Entry<String, JsonElement> param : params.entrySet())
                    cell.params.put(param.getKey(), param.getValue().getAsString());

                cells.put(entry.getKey(), cell);
            }
        }
    }

    public static String escape_name(String name) {
        return name.replace("\\", "__").replace("/", "_");
    }

    public static void main(String[] args) throws FileNotFoundException {

        if (args.length < 3) {
            System.err.println("Usage: json2dcp <device> <design.json> <design.dcp>");
            System.err.println("   e.g json2dcp xczu2cg-sbva484-1-e top_routed.json top_routed.dcp");
            System.exit(1);
        }

        NextpnrDesign ndes = new NextpnrDesign();
        ndes.Import(new JsonParser().parse(new FileReader(args[1])).getAsJsonObject());

        Design des = new Design("top", args[0]);

        for (NextpnrCell nc : ndes.cells.values()) {
            if (!nc.attrs.containsKey("X_ORIG_TYPE"))
                continue;
            if (nc.type.equals("IOB_OUTBUF") || nc.type.equals("IOB_IBUFCTRL"))
                continue;
            nc.rwCell = des.createAndPlaceCell(nc.name, Unisim.valueOf(nc.attrs.get("X_ORIG_TYPE")), nc.attrs.get("NEXTPNR_BEL"));
        }

        for (NextpnrNet nn : ndes.nets.values()) {
            //System.out.println("create net " + nn.name);
            Net n = des.createNet(escape_name(nn.name));
            nn.rwNet = n;
            if (nn.driver != null && nn.driver.cell.rwCell != null) {
                if (!nn.driver.cell.attrs.containsKey("X_ORIG_PORT_" + nn.driver.name))
                    continue;
                //System.out.println("connect " + n.getName() + " <- " + nn.driver.cell.name + "." + nn.driver.name);
                n.connect(nn.driver.cell.rwCell, nn.driver.cell.attrs.get("X_ORIG_PORT_" + nn.driver.name));
            }
            for (NextpnrCellPort usr : nn.users) {
                if (usr.cell.rwCell != null) {
                    if (!usr.cell.attrs.containsKey("X_ORIG_PORT_" + usr.name))
                        continue;
                    //System.out.println("connect " + n.getName() + " -> " + usr.cell.name + "." + usr.name);
                    n.connect(usr.cell.rwCell, usr.cell.attrs.get("X_ORIG_PORT_" + usr.name));
                }
            }

            String[] routing = nn.attrs.get("ROUTING").split(";");
            for (int i = 0; i < routing.length; i+=3) {
                String wire = routing[i];
                String pip = routing[i+1];
                if (pip.isEmpty() || pip.startsWith("SITEPIP"))
                    continue;
                String[] sp = pip.split("/");
                String tile = sp[0];
                String[] wires = sp[1].split("\\.");
                Tile t = des.getDevice().getTile(tile);
                int src = Integer.parseInt(wires[0]), dst = Integer.parseInt(wires[1]);
                if (src < t.getWireCount() && dst < t.getWireCount())
                    n.addPIP(t.getPIP(src, dst));
            }

        }

        des.writeCheckpoint(args[2]);
    }

}
